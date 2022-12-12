/** Expression generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
#include "../shared/fileio.h"
#include "genllvm.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/Scalar.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

LLVMValueRef genlAddr(GenState *gen, INode *lval);

// Generate an if statement
LLVMValueRef genlIf(GenState *gen, IfNode *ifnode) {
    LLVMBasicBlockRef endif;
    LLVMBasicBlockRef nextif;
    INode *vtype;
    int i, phicnt, count;
    LLVMValueRef *blkvals;
    LLVMBasicBlockRef *blks;
    INode **nodesp;
    uint32_t cnt;

    // If we are returning a value in each block, set up space for phi info
    vtype = itypeGetTypeDcl(ifnode->vtype);
    count = ifnode->condblk->used / 2;
    i = phicnt = 0;
    if (vtype != unknownType) {
        blkvals = memAllocBlk(count * sizeof(LLVMValueRef));
        blks = memAllocBlk(count * sizeof(LLVMBasicBlockRef));
    }

    endif = genlInsertBlock(gen, "endif");
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        // Set up block for next condition (or endif if this is last condition)
        if (i + 1 < count)
            nextif = LLVMInsertBasicBlockInContext(gen->context, endif, "ifnext");
        else
            nextif = endif;

        // Set up this condition's statement block and then conditionally jump to it or next condition
        LLVMBasicBlockRef ablk;
        if (*nodesp != elseCond) {
            ablk = LLVMInsertBasicBlockInContext(gen->context, nextif, "ifblk");
            LLVMBuildCondBr(gen->builder, genlExpr(gen, *nodesp), ablk, nextif);
            LLVMPositionBuilderAtEnd(gen->builder, ablk);
        }

        // Generate this condition's code block, along with jump to endif if block does not end with a return
        LLVMValueRef blkval = genlBlock(gen, (BlockNode*)*(nodesp + 1));
        uint16_t lastStmttype = nodesLast(((BlockNode*)*(nodesp + 1))->stmts)->tag;
        if (lastStmttype != ReturnTag && lastStmttype != BreakTag && lastStmttype != ContinueTag) {
            LLVMBuildBr(gen->builder, endif);
            // Remember value and block if needed for phi merge
            if (vtype != unknownType) {
                blkvals[phicnt] = blkval;
                blks[phicnt++] = LLVMGetInsertBlock(gen->builder);
            }
        }

        LLVMPositionBuilderAtEnd(gen->builder, nextif);
        cnt--; nodesp++; i++;
    }

    // Merge point at end of if. Create merged phi value if needed.
    if (phicnt) {
        LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, vtype), "ifval");
        LLVMAddIncoming(phi, blkvals, blks, phicnt);
        return phi;
    }

    return NULL;
}

// Obtain value ref for a specific named intrinsic function
LLVMValueRef genlGetIntrinsicFn(GenState *gen, char *fnname, NameUseNode *fnuse) {
    LLVMValueRef fn = LLVMGetNamedFunction(gen->module, fnname);
    if (!fn)
        fn = LLVMAddFunction(gen->module, fnname, genlType(gen, iexpGetTypeDcl((INode*)fnuse->dclnode)));
    return fn;
}

// Generate a function call, including special intrinsics (Internal version)
LLVMValueRef genlFnCallInternal(GenState *gen, int dispatch, INode *objfn, uint32_t fnargcnt, LLVMValueRef *fnargs) {

    // Handle call when we have a derefed pointer to a function
    if (objfn->tag == DerefTag) {
        return LLVMBuildCall(gen->builder, genlExpr(gen, ((StarNode*)objfn)->vtexp), fnargs, fnargcnt, "");
    }
    // Handle call when we have a ref or pointer to a function
    INode *fntype = iexpGetTypeDcl(objfn);
    if (fntype->tag == RefTag || fntype->tag == PtrTag) {
        return LLVMBuildCall(gen->builder, genlExpr(gen, objfn), fnargs, fnargcnt, "");
    }

    // We know at this point that objfn refers to some "named" function
    assert(fntype->tag == FnSigTag);
    // Handle call when first argument (object) is a virtual reference
    if (dispatch == VirtDispatch) {
        // Build code to obtain method's address from object "fat pointer"'s vtable, then call it
        LLVMValueRef fatptr = fnargs[0];
        LLVMValueRef vtable = LLVMBuildExtractValue(gen->builder, fatptr, 1, "");
        fnargs[0] = LLVMBuildExtractValue(gen->builder, fatptr, 0, "");
        FnDclNode *methdcl = (FnDclNode*)((NameUseNode *)objfn)->dclnode;
        LLVMValueRef vtblmethp = LLVMBuildStructGEP(gen->builder, vtable, methdcl->vtblidx, &methdcl->namesym->namestr); // **fn
        LLVMValueRef vtblmeth = LLVMBuildLoad(gen->builder, vtblmethp, "");
        return LLVMBuildCall(gen->builder, vtblmeth, fnargs, fnargcnt, "");
    }

    // A function call may be to an intrinsic, or to program-defined code
    NameUseNode *fnuse;
    FnDclNode *fndcl;
    if (objfn->tag == FnDclTag) {
        fnuse = NULL; // Hmmm
        fndcl = (FnDclNode *)objfn;
    }
    else {
        assert(objfn->tag == VarNameUseTag);
        fnuse = (NameUseNode *)objfn;
        fndcl = (FnDclNode *)fnuse->dclnode;
    }

    if (fndcl->flags & FlagInline) {
        // For inline functions, first generate call args as local "parameter" variables
        if (fnargcnt > 0) {
            FnSigNode *fnsig = (FnSigNode*)fndcl->vtype;
            uint32_t cnt;
            INode **nodesp;
            for (nodesFor(fnsig->parms, cnt, nodesp)) {
                VarDclNode* var = (VarDclNode*)*nodesp;
                assert(var->tag == VarDclTag);
                // We always alloca in case variable is mutable or we want to take address of its value
                var->llvmvar = genlAlloca(gen, genlType(gen, var->vtype), &var->namesym->namestr);
                LLVMBuildStore(gen->builder, *fnargs++, var->llvmvar);
            }
        }

        // Now generate function's block, as if any block, returning block's value
        return genlBlock(gen, (BlockNode *)fndcl->value);
    }

    LLVMValueRef fncallret = NULL;
    switch (fndcl->value? fndcl->value->tag : BlockTag) {
    case BlockTag: {
        fncallret = LLVMBuildCall(gen->builder, fndcl->llvmvar, fnargs, fnargcnt, "");
        if (fndcl->flags & FlagSystem) {
            LLVMSetInstructionCallConv(fncallret, LLVMX86StdcallCallConv);
        }
        break;
    }
    case IntrinsicTag: {
        // Logic to generate depends on type of first argument
        LLVMTypeRef selftyp = LLVMTypeOf(fnargs[0]);
        LLVMTypeKind selftypkind = LLVMGetTypeKind(selftyp);

        // Pointer intrinsics
        if (selftypkind == LLVMPointerTypeKind) {
            LLVMTypeRef ptrToType = LLVMGetElementType(selftyp);
            LLVMTypeKind ptrToKind = LLVMGetTypeKind(ptrToType);
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
                // Comparison
            case IsTrueIntrinsic: fncallret = LLVMBuildIsNotNull(gen->builder, fnargs[0], "isnotnull"); break;
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            case LtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], ""); break;
            case LeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], ""); break;
            case GtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], ""); break;
            case GeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], ""); break;
            case IncrIntrinsic: // ++x
            {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                if (ptrToKind == LLVMPointerTypeKind) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
                    fncallret = LLVMBuildGEP(gen->builder, val, &constone, 1, "");
                }
                else if (ptrToKind == LLVMFloatTypeKind)
                    fncallret = LLVMBuildFAdd(gen->builder, val, LLVMConstReal(ptrToType, 1.), "");
                else // LLVMIntegerTypeKind
                    fncallret = LLVMBuildAdd(gen->builder, val, LLVMConstInt(ptrToType, 1, 0), "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;
            }
            case DecrIntrinsic: // --x
            {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                if (ptrToKind == LLVMPointerTypeKind) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), -1, 1);
                    fncallret = LLVMBuildGEP(gen->builder, val, &constone, 1, "");
                }
                else if (ptrToKind == LLVMFloatTypeKind)
                    fncallret = LLVMBuildFSub(gen->builder, val, LLVMConstReal(ptrToType, 1.), "");
                else // LLVMIntegerTypeKind
                    fncallret = LLVMBuildSub(gen->builder, val, LLVMConstInt(ptrToType, 1, 0), "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;
            }
            case IncrPostIntrinsic: // x++
            {
                fncallret = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMValueRef val;
                if (ptrToKind == LLVMPointerTypeKind) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
                    val = LLVMBuildGEP(gen->builder, fncallret, &constone, 1, "");
                }
                else if (ptrToKind == LLVMFloatTypeKind)
                    val = LLVMBuildFAdd(gen->builder, fncallret, LLVMConstReal(ptrToType, 1.), "");
                else // LLVMIntegerTypeKind
                    val = LLVMBuildAdd(gen->builder, fncallret, LLVMConstInt(ptrToType, 1, 0), "");
                LLVMBuildStore(gen->builder, val, fnargs[0]);
                break;
            }
            case DecrPostIntrinsic: // x--
            {
                fncallret = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMValueRef val;
                if (ptrToKind == LLVMPointerTypeKind) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), -1, 1);
                    val = LLVMBuildGEP(gen->builder, fncallret, &constone, 1, "");
                }
                else if (ptrToKind == LLVMFloatTypeKind)
                    val = LLVMBuildFSub(gen->builder, fncallret, LLVMConstReal(ptrToType, 1.), "");
                else // LLVMIntegerTypeKind
                    val = LLVMBuildSub(gen->builder, fncallret, LLVMConstInt(ptrToType, 1, 0), "");
                LLVMBuildStore(gen->builder, val, fnargs[0]);
                break;
            }
            case AddIntrinsic: fncallret = LLVMBuildGEP(gen->builder, fnargs[0], &fnargs[1], 1, ""); break;
            case SubIntrinsic: {
                LLVMValueRef negval = LLVMBuildNeg(gen->builder, fnargs[1], "");
                fncallret = LLVMBuildGEP(gen->builder, fnargs[0], &negval, 1, ""); 
                break;
            }
            case DiffIntrinsic: {
                LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
                LLVMValueRef selfint = LLVMBuildPtrToInt(gen->builder, fnargs[0], usize, "");
                LLVMValueRef argint = LLVMBuildPtrToInt(gen->builder, fnargs[1], usize, "");
                LLVMValueRef diff = LLVMBuildSub(gen->builder, selfint, argint, "");
                long long valsize = LLVMABISizeOfType(gen->datalayout, ptrToType);
                fncallret = LLVMBuildSDiv(gen->builder, diff, LLVMConstInt(usize, valsize, 1), "");
                break;
            }
            case AddEqIntrinsic: {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                fncallret = LLVMBuildGEP(gen->builder, val, &fnargs[1], 1, "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;

            }
            case SubEqIntrinsic: {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMValueRef negval = LLVMBuildNeg(gen->builder, fnargs[1], "");
                fncallret = LLVMBuildGEP(gen->builder, val, &negval, 1, "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;

            }
            }
        }

        // Array reference intrinsics (fatptr is a struct)
        else if (selftypkind == LLVMStructTypeKind) {
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
            case CountIntrinsic: fncallret = LLVMBuildExtractValue(gen->builder, fnargs[0], 1, "slicecount"); break;
            // Comparison
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            }
        }

        // Floating point intrinsics
        else if (selftypkind == LLVMFloatTypeKind || selftypkind == LLVMDoubleTypeKind) {
            unsigned long long bitwidth = LLVMABISizeOfType(gen->datalayout, selftyp);
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
            case NegIntrinsic: fncallret = LLVMBuildFNeg(gen->builder, fnargs[0], ""); break;
            case IsTrueIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealONE, fnargs[0], LLVMConstNull(LLVMTypeOf(fnargs[0])), ""); break;
            case AddIntrinsic: fncallret = LLVMBuildFAdd(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SubIntrinsic: fncallret = LLVMBuildFSub(gen->builder, fnargs[0], fnargs[1], ""); break;
            case MulIntrinsic: fncallret = LLVMBuildFMul(gen->builder, fnargs[0], fnargs[1], ""); break;
            case DivIntrinsic: fncallret = LLVMBuildFDiv(gen->builder, fnargs[0], fnargs[1], ""); break;
            case RemIntrinsic: fncallret = LLVMBuildFRem(gen->builder, fnargs[0], fnargs[1], ""); break;
            // Comparison
            case EqIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealOEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealONE, fnargs[0], fnargs[1], ""); break;
            case LtIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealOLT, fnargs[0], fnargs[1], ""); break;
            case LeIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealOLE, fnargs[0], fnargs[1], ""); break;
            case GtIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealOGT, fnargs[0], fnargs[1], ""); break;
            case GeIntrinsic: fncallret = LLVMBuildFCmp(gen->builder, LLVMRealOGE, fnargs[0], fnargs[1], ""); break;
            // Intrinsic functions
            case SqrtIntrinsic: 
            {
                char *fnname = bitwidth == 32 ? "llvm.sqrt.f32" : "llvm.sqrt.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fnargcnt, "");
                break;
            }
            case SinIntrinsic:
            {
                char *fnname = bitwidth == 32 ? "llvm.sin.f32" : "llvm.sin.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fnargcnt, "");
                break;
            }
            case CosIntrinsic:
            {
                char *fnname = bitwidth == 32 ? "llvm.cos.f32" : "llvm.cos.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fnargcnt, "");
                break;
            }
            }
        }
        // Signed and Unsigned Integer intrinsics
        else {
            assert(selftypkind == LLVMIntegerTypeKind);
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {

                // Arithmetic
            case NegIntrinsic: fncallret = LLVMBuildNeg(gen->builder, fnargs[0], ""); break;
            case IsTrueIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], LLVMConstNull(LLVMTypeOf(fnargs[0])), ""); break;
            case AddIntrinsic: fncallret = LLVMBuildAdd(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SubIntrinsic: fncallret = LLVMBuildSub(gen->builder, fnargs[0], fnargs[1], ""); break;
            case MulIntrinsic: fncallret = LLVMBuildMul(gen->builder, fnargs[0], fnargs[1], ""); break;
            case DivIntrinsic: fncallret = LLVMBuildUDiv(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SDivIntrinsic: fncallret = LLVMBuildSDiv(gen->builder, fnargs[0], fnargs[1], ""); break;
            case RemIntrinsic: fncallret = LLVMBuildURem(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SRemIntrinsic: fncallret = LLVMBuildSRem(gen->builder, fnargs[0], fnargs[1], ""); break;

                // Comparison
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            case LtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], ""); break;
            case SLtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntSLT, fnargs[0], fnargs[1], ""); break;
            case LeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], ""); break;
            case SLeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntSLE, fnargs[0], fnargs[1], ""); break;
            case GtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], ""); break;
            case SGtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntSGT, fnargs[0], fnargs[1], ""); break;
            case GeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], ""); break;
            case SGeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntSGE, fnargs[0], fnargs[1], ""); break;

            // Bitwise
            case NotIntrinsic: fncallret = LLVMBuildNot(gen->builder, fnargs[0], ""); break;
            case AndIntrinsic: fncallret = LLVMBuildAnd(gen->builder, fnargs[0], fnargs[1], ""); break;
            case OrIntrinsic: fncallret = LLVMBuildOr(gen->builder, fnargs[0], fnargs[1], ""); break;
            case XorIntrinsic: fncallret = LLVMBuildXor(gen->builder, fnargs[0], fnargs[1], ""); break;
            case ShlIntrinsic: fncallret = LLVMBuildShl(gen->builder, fnargs[0], fnargs[1], ""); break;
            case ShrIntrinsic: fncallret = LLVMBuildLShr(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SShrIntrinsic: fncallret = LLVMBuildAShr(gen->builder, fnargs[0], fnargs[1], ""); break;
            }
        }
        break;
    }
    default:
        assert(0 && "invalid type of function call");
    }

    return fncallret;
}

// Generate a function call, including special intrinsics
LLVMValueRef genlFnCall(GenState *gen, FnCallNode *fncall) {

    int dispatch;
    if (fncall->flags & FlagVDisp)
        dispatch = VirtDispatch;
    else
        dispatch = SimpleDispatch;

    INode *objfn = fncall->objfn;

    // Get count and Valuerefs for all the arguments to pass to the function
    uint32_t fnargcnt = fncall->args->used;
    LLVMValueRef *fnargs = (LLVMValueRef*)memAllocBlk(fnargcnt * sizeof(LLVMValueRef*));
    LLVMValueRef *fnarg = fnargs;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fncall->args, cnt, nodesp)) {
        *fnarg++ = genlExpr(gen, *nodesp);
    }

    return genlFnCallInternal(gen, dispatch, objfn, fnargcnt, fnargs);
}

// Generate a value converted to another type
// - Numbers to another number (extending or truncating)
// - Number or ref/ptr to true/false
// - Array ref to ptr or size
LLVMValueRef genlConvert(GenState *gen, INode* exp, INode* to) {
    INode *fromtype = iexpGetTypeDcl(exp);
    INode *totype = itypeGetTypeDcl(to);
    LLVMValueRef genexp = genlExpr(gen, exp);

    // Handle number to number casts, depending on relative size and encoding format
    switch (totype->tag) {

    case UintNbrTag:
        if (fromtype->tag == FloatNbrTag)
            return LLVMBuildFPToUI(gen->builder, genexp, genlType(gen, totype), "");
        else if (((NbrNode*)totype)->bits < ((NbrNode*)fromtype)->bits)
            return LLVMBuildTrunc(gen->builder, genexp, genlType(gen, totype), "");
        else /* if (((NbrNode*)totype)->bits >((NbrNode*)fromtype)->bits) */
            return LLVMBuildZExt(gen->builder, genexp, genlType(gen, totype), "");

    case IntNbrTag:
        if (fromtype->tag == FloatNbrTag)
            return LLVMBuildFPToSI(gen->builder, genexp, genlType(gen, totype), "");
        else if (((NbrNode*)totype)->bits < ((NbrNode*)fromtype)->bits)
            return LLVMBuildTrunc(gen->builder, genexp, genlType(gen, totype), "");
        else /*if (((NbrNode*)totype)->bits >((NbrNode*)fromtype)->bits) */ {
            if (fromtype->tag == IntNbrTag)
                return LLVMBuildSExt(gen->builder, genexp, genlType(gen, totype), "");
            else
                return LLVMBuildZExt(gen->builder, genexp, genlType(gen, totype), "");
        }

    case FloatNbrTag:
        if (fromtype->tag == IntNbrTag)
            return LLVMBuildSIToFP(gen->builder, genexp, genlType(gen, totype), "");
        else if (fromtype->tag == UintNbrTag)
            return LLVMBuildUIToFP(gen->builder, genexp, genlType(gen, totype), "");
        else if (((NbrNode*)totype)->bits < ((NbrNode*)fromtype)->bits)
            return LLVMBuildFPTrunc(gen->builder, genexp, genlType(gen, totype), "");
        else if (((NbrNode*)totype)->bits >((NbrNode*)fromtype)->bits)
            return LLVMBuildFPExt(gen->builder, genexp, genlType(gen, totype), "");
        else
            return genexp;

    case StructTag:
    {
        // LLVM does not bitcast structs, so this store/load hack gets around that problem
        LLVMValueRef tempspaceptr = LLVMBuildAlloca(gen->builder, genlType(gen, fromtype), "");
        LLVMValueRef store = LLVMBuildStore(gen->builder, genexp, tempspaceptr);
        LLVMValueRef castptr = LLVMBuildBitCast(gen->builder, tempspaceptr, LLVMPointerType(genlType(gen, totype), 0), "");
        return LLVMBuildLoad(gen->builder, castptr, "");
    }

    case RefTag:
    {
        // extract object pointer from fat pointer
        if (fromtype->tag == VirtRefTag) {
            LLVMValueRef ptr = LLVMBuildExtractValue(gen->builder, genexp, 0, "ref");
            return LLVMBuildBitCast(gen->builder, ptr, genlType(gen, totype), "");
        }
        else if (fromtype->tag == RefTag)
            return LLVMBuildBitCast(gen->builder, genexp, genlType(gen, totype), "");
        else
            assert(0 && "Unknown type to convert to reference");
    }

    case ArrayRefTag:
    {
        // Convert ref-to-array into arrayref
        if (fromtype->tag == RefTag) {
            ArrayNode *arraytype = (ArrayNode*)((RefNode *)fromtype)->vtexp;
            assert(arraytype->tag == ArrayTag);
            LLVMValueRef aref = LLVMGetUndef(genlType(gen, totype));
            LLVMValueRef size = LLVMConstInt(genlUsize(gen), arrayDim1((INode*)arraytype), 0);
            LLVMTypeRef elemtype = genlType(gen, nodesGet(arraytype->elems, 0));
            LLVMTypeRef recasttype = LLVMPointerType(elemtype, 0);
            LLVMValueRef genexpcast = LLVMBuildBitCast(gen->builder, genexp, recasttype, "");
            aref = LLVMBuildInsertValue(gen->builder, aref, genexpcast, 0, "arrayp");
            aref = LLVMBuildInsertValue(gen->builder, aref, size, 1, "size");
            return aref;
        }
        else
            assert(0 && "Unknown type to convert to array reference");
    }

    case VirtRefTag:
    {
        // Build a fat ptr whose vtable maps the fromtype struct to the totype trait
        assert(fromtype->tag == RefTag);
        StructNode *trait = (StructNode*)itypeGetTypeDcl(((RefNode*)totype)->vtexp);
        StructNode *strnode = (StructNode*)itypeGetTypeDcl(((RefNode*)fromtype)->vtexp);
        Vtable *vtable = ((StructNode*)trait)->vtable;
        if (vtable->llvmvtable == NULL)
            genlVtable(gen, vtable);

        LLVMValueRef vtablep;
        if (!(strnode->flags & TraitType)) {
            VtableImpl *impl;
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(vtable->impl, cnt, nodesp)) {
                impl = (VtableImpl*)*nodesp;
                if (impl->structdcl == (INode*)strnode) {
                    vtablep = impl->llvmvtablep;
                    break;
                }
            }
        }
        else {
            // Use tag field to lookup correct vtable
            INode **nodesp;
            uint32_t cnt;
            for (nodelistFor(&strnode->fields, cnt, nodesp)) {
                if ((*nodesp)->flags & IsTagField) {
                    FieldDclNode *tagnode = (FieldDclNode*)*nodesp;
                    LLVMValueRef val = LLVMBuildStructGEP(gen->builder, genexp, tagnode->index, "tagref");
                    val = LLVMBuildLoad(gen->builder, val, "tag");
                    LLVMValueRef indexes[2];
                    indexes[0] = LLVMConstInt(genlUsize(gen), 0, 0);
                    indexes[1] = val;
                    vtablep = LLVMBuildGEP(gen->builder, vtable->llvmvtables, indexes, 2, "");
                    vtablep = LLVMBuildLoad(gen->builder, vtablep, "");
                }
            }
        }
        LLVMValueRef vref = LLVMGetUndef(genlType(gen, totype));
        LLVMTypeRef vptr = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
        LLVMValueRef ptr = LLVMBuildBitCast(gen->builder, genexp, vptr, "");
        vref = LLVMBuildInsertValue(gen->builder, vref, ptr, 0, "vptr");
        vref = LLVMBuildInsertValue(gen->builder, vref, vtablep, 1, "vptr");
        return vref;
    }

    case PtrTag:
        if (fromtype->tag == ArrayRefTag)
            return LLVMBuildExtractValue(gen->builder, genexp, 0, "sliceptr");
        else
            return LLVMBuildBitCast(gen->builder, genexp, genlType(gen, totype), "");

    default:
        if (totype->tag == StructTag) {
            // LLVM does not bitcast structs, so this store/load hack gets around that problem
            LLVMValueRef tempspaceptr = genlAlloca(gen, genlType(gen, fromtype), "");
            LLVMValueRef store = LLVMBuildStore(gen->builder, genexp, tempspaceptr);
            LLVMValueRef castptr = LLVMBuildBitCast(gen->builder, tempspaceptr, LLVMPointerType(genlType(gen, totype),0), "");
            return LLVMBuildLoad(gen->builder, castptr, "");
        }
        return LLVMBuildBitCast(gen->builder, genexp, genlType(gen, totype), "");
    }
}

// Reinterpret a value as if it were another type
LLVMValueRef genlRecast(GenState *gen, INode* exp, INode* to) {
    INode *totype = itypeGetTypeDcl(to);
    LLVMValueRef genexp = genlExpr(gen, exp);
    if (totype->tag == StructTag) {
        // LLVM does not bitcast structs, so this store/load hack gets around that problem
        LLVMValueRef tempspaceptr = genlAlloca(gen, genlType(gen, ((IExpNode*)exp)->vtype), "");
        LLVMValueRef store = LLVMBuildStore(gen->builder, genexp, tempspaceptr);
        LLVMValueRef castptr = LLVMBuildBitCast(gen->builder, tempspaceptr, LLVMPointerType(genlType(gen, totype), 0), "");
        return LLVMBuildLoad(gen->builder, castptr, "");
    }
    return LLVMBuildBitCast(gen->builder, genexp, genlType(gen, totype), "");
}

// Generate not
LLVMValueRef genlNot(GenState *gen, LogicNode* node) {
    return LLVMBuildXor(gen->builder, genlExpr(gen, node->lexp), LLVMConstInt(LLVMInt1TypeInContext(gen->context), 1, 0), "not");
}

// Generate and, or
LLVMValueRef genlLogic(GenState *gen, LogicNode* node) {
    LLVMBasicBlockRef logicblks[2];
    LLVMValueRef logicvals[2];

    // Set up basic blocks
    LLVMBasicBlockRef logicphi = genlInsertBlock(gen, node->tag==AndLogicTag? "andphi" : "orphi");
    LLVMBasicBlockRef logicrhs = genlInsertBlock(gen, node->tag == AndLogicTag ? "andrhs" : "orrhs");

    // Generate left-hand condition and conditional branch
    logicvals[0] = genlExpr(gen, node->lexp);
    if (node->tag==OrLogicTag)
        LLVMBuildCondBr(gen->builder, logicvals[0], logicphi, logicrhs);
    else
        LLVMBuildCondBr(gen->builder, logicvals[0], logicrhs, logicphi);
    logicblks[0] = LLVMGetInsertBlock(gen->builder);

    // Generate right-hand condition and branch to phi
    LLVMPositionBuilderAtEnd(gen->builder, logicrhs);
    logicvals[1] = genlExpr(gen, node->rexp);
    LLVMBuildBr(gen->builder, logicphi);
    logicblks[1] = LLVMGetInsertBlock(gen->builder);

    // Generate phi
    LLVMPositionBuilderAtEnd(gen->builder, logicphi);
    LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, (INode*)boolType), "logicval");
    LLVMAddIncoming(phi, logicvals, logicblks, 2);
    return phi;
}

// Generate local variable
LLVMValueRef genlLocalVar(GenState *gen, VarDclNode *var) {
    assert(var->tag == VarDclTag);
    LLVMValueRef val = NULL;
    var->llvmvar = genlAlloca(gen, genlType(gen, var->vtype), &var->namesym->namestr);
    if (var->value) {
        val = genlExpr(gen, var->value);
        LLVMBuildStore(gen->builder, val, var->llvmvar);
    }
    return val;
}

// Returns a boolean indicating whether some value may be downcast to the specialized type
LLVMValueRef genlIsType(GenState *gen, CastNode *isnode) {
    LLVMValueRef val = genlExpr(gen, isnode->exp);
    INode *exptype = iexpGetTypeDcl(isnode->exp);
    INode *istype = itypeGetTypeDcl(isnode->typ);
    StructNode *structtype = (StructNode*)(istype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)istype)->vtexp) : istype);

    // If pattern matching a virtual reference, compare vtable pointers
    if (exptype->tag == VirtRefTag) {
        LLVMValueRef vtablep = LLVMBuildExtractValue(gen->builder, val, 1, "");
        Vtable *vtable = structGetBaseTrait(structtype)->vtable;
        if (vtable->llvmvtable == NULL)
            genlVtable(gen, vtable);

        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(vtable->impl, cnt, nodesp)) {
            VtableImpl *impl = (VtableImpl*)*nodesp;
            if (impl->structdcl == (INode*)structtype) {
                LLVMValueRef diff = LLVMBuildPtrDiff(gen->builder, vtablep, impl->llvmvtablep, "");
                LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(gen->context), 0, 0);
                return LLVMBuildICmp(gen->builder, LLVMIntEQ, diff, zero, "isvtable");
            }
        }
        assert(0 && "Could not find specialized type's vtable");
    }

    // Special handling for nullable pointers
    if (structtype->flags & NullablePtr) {
        LLVMTypeRef ptrtype = structtype->llvmtype;
        if (LLVMGetTypeKind(ptrtype) != LLVMPointerTypeKind)
            val = LLVMBuildExtractValue(gen->builder, val, 0, "ptr"); // VirtRef & ArrayRef
        LLVMValueRef nullptr = LLVMConstPointerNull(ptrtype);
        LLVMIntPredicate cmpop = structtype->fields.used == 1 ? LLVMIntEQ : LLVMIntNE;
        return LLVMBuildICmp(gen->builder, cmpop, val, nullptr, "isnull");
    }

    // Pattern match whether termnode's tag matches desired concrete struct type
    // Find and extract tag field from val, then compare with to-type's tag number
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&structtype->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsTagField) {
            FieldDclNode *tagnode = (FieldDclNode*)*nodesp;
            if (istype->tag == RefTag) {
                val = LLVMBuildStructGEP(gen->builder, val, tagnode->index, "tagref");
                val = LLVMBuildLoad(gen->builder, val, "tag");
            }
            else
                val = LLVMBuildExtractValue(gen->builder, val, tagnode->index, "tag");
            LLVMValueRef tagval = LLVMConstInt(genlType(gen, tagnode->vtype), structtype->tagnbr, 0);
            return LLVMBuildICmp(gen->builder, LLVMIntEQ, val, tagval, "istag");
        }
    }
    assert(0 && "Should not reach here, because type check ensures type has a tag field");
    return NULL;
}

// Generate a panic
void genlPanic(GenState *gen) {
    // Declare trap() external function
    char *fnname = "llvm.trap";
    LLVMValueRef fn = LLVMGetNamedFunction(gen->module, fnname);
    if (!fn) {
        LLVMTypeRef rettype = LLVMVoidTypeInContext(gen->context);
        LLVMTypeRef fnsig = LLVMFunctionType(rettype, NULL, 0, 0);
        fn = LLVMAddFunction(gen->module, fnname, fnsig);
    }
    LLVMBuildCall(gen->builder, fn, NULL, 0, "");
}

void genlBoundsCheck(GenState *gen, LLVMValueRef index, LLVMValueRef count) {
    // Do runtime bounds check and panic
    LLVMBasicBlockRef panicblk = genlInsertBlock(gen, "panic");
    LLVMBasicBlockRef boundsblk = genlInsertBlock(gen, "boundsok");
    LLVMValueRef compare = LLVMBuildICmp(gen->builder, LLVMIntULT, index, count, "");
    LLVMBuildCondBr(gen->builder, compare, boundsblk, panicblk);
    LLVMPositionBuilderAtEnd(gen->builder, panicblk);
    genlPanic(gen);
    LLVMBuildBr(gen->builder, boundsblk);
    LLVMPositionBuilderAtEnd(gen->builder, boundsblk);
}

LLVMValueRef genlArrayIndex(GenState *gen, FnCallNode *fncall, ArrayNode *objtype) {
    // Allocate a indexing buffer for GEP
    LLVMValueRef indexes[2];
    LLVMValueRef *indexp = &indexes[0];
    uint16_t nindex = objtype->dimens->used;
    if (nindex > 1)
        indexp = memAllocBlk(nindex * sizeof(LLVMValueRef));
    indexp[0] = LLVMConstInt(genlUsize(gen), 0, 0);
    
    // Populate indexing buffer
    for (int arg = 0; arg < nindex; arg++) {
        ULitNode *dimen = (ULitNode*)nodesGet(objtype->dimens, arg);
        assert(dimen->tag == ULitTag);
        LLVMValueRef count = LLVMConstInt(genlUsize(gen), dimen->uintlit, 0);
        LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, arg));
        genlBoundsCheck(gen, index, count);
        indexp[arg+1] = index;
    }
    return LLVMBuildGEP(gen->builder, genlAddr(gen, fncall->objfn), indexp, nindex+1, "");
}

// Generate an lval-ish pointer to the value (vs. load)
LLVMValueRef genlAddr(GenState *gen, INode *lval) {
    switch (lval->tag) {
    case FnDclTag:
        genlGloFnName(gen, (FnDclNode *)lval);
        genlFn(gen, (FnDclNode*)lval);
        return ((FnDclNode*)lval)->llvmvar;
    case VarNameUseTag:
        return ((VarDclNode*)((NameUseNode *)lval)->dclnode)->llvmvar;
    case DerefTag:
        return genlExpr(gen, ((StarNode *)lval)->vtexp);
    case ArrIndexTag:
    {
        FnCallNode *fncall = (FnCallNode *)lval;
        INode *objtype = iexpGetTypeDcl(fncall->objfn);
        switch (objtype->tag) {
        case ArrayTag: {
            return genlArrayIndex(gen, fncall, (ArrayNode*)objtype);
        }
        case ArrayRefTag: {
            LLVMValueRef arrref = genlExpr(gen, fncall->objfn);
            LLVMValueRef count = LLVMBuildExtractValue(gen->builder, arrref, 1, "count");
            LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
            genlBoundsCheck(gen, index, count);
            LLVMValueRef sliceptr = LLVMBuildExtractValue(gen->builder, arrref, 0, "sliceptr");
            return LLVMBuildGEP(gen->builder, sliceptr, &index, 1, "");
        }
        case ArrayDerefTag: {
            StarNode *deref = (StarNode *)fncall->objfn;
            assert(deref->tag == DerefTag);
            LLVMValueRef arrref = genlExpr(gen, deref->vtexp);
            LLVMValueRef count = LLVMBuildExtractValue(gen->builder, arrref, 1, "count");
            LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
            genlBoundsCheck(gen, index, count);
            LLVMValueRef sliceptr = LLVMBuildExtractValue(gen->builder, arrref, 0, "sliceptr");
            return LLVMBuildGEP(gen->builder, sliceptr, &index, 1, "");
        }
        case PtrTag: {
            LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
            return LLVMBuildGEP(gen->builder, genlExpr(gen, fncall->objfn), &index, 1, "");
        }
        default:
            assert(0 && "Unknown type of arrindex element indexing node");
        }
    }
    case FldAccessTag:
    {
        FnCallNode *fncall = (FnCallNode *)lval;
        if (fncall->methfld->tag == MbrNameUseTag) {
            FieldDclNode *flddcl = (FieldDclNode*)((NameUseNode*)fncall->methfld)->dclnode;
            if (iexpGetTypeDcl(fncall->objfn)->tag == VirtRefTag) {
                // Calculate address of virtual field pointed to by a virtual reference using vtable
                LLVMValueRef objVRef = genlExpr(gen, fncall->objfn);
                LLVMValueRef objpRef = LLVMBuildExtractValue(gen->builder, objVRef, 0, ""); // *u8
                LLVMValueRef vtable = LLVMBuildExtractValue(gen->builder, objVRef, 1, "");
                LLVMValueRef vtblfldp = LLVMBuildStructGEP(gen->builder, vtable, flddcl->vtblidx, &flddcl->namesym->namestr); // *u32
                LLVMValueRef vtblfld = LLVMBuildLoad(gen->builder, vtblfldp, "");
                LLVMValueRef fldpRef = LLVMBuildGEP(gen->builder, objpRef, &vtblfld, 1, "");
                return LLVMBuildBitCast(gen->builder, fldpRef, LLVMPointerType(genlType(gen, flddcl->vtype), 0), "");
            }
            return LLVMBuildStructGEP(gen->builder, genlAddr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
        }
        else if (fncall->methfld->tag == UintNbrTag) {
            ULitNode *ulit = (ULitNode*)fncall->methfld;
            return LLVMBuildStructGEP(gen->builder, genlAddr(gen, fncall->objfn), (unsigned int)ulit->uintlit, "");
        }
        else
            assert(0 && "Invalid FldAccess methfld.");
    }
    case StringLitTag:
    {
        SLitNode *strnode = (SLitNode *)lval;
        LLVMValueRef sglobal = LLVMAddGlobal(gen->module, genlType(gen, strnode->vtype), "string");
        LLVMSetLinkage(sglobal, LLVMInternalLinkage);
        LLVMSetGlobalConstant(sglobal, 1);
        LLVMSetInitializer(sglobal, LLVMConstStringInContext(gen->context, strnode->strlit, strnode->strlen, 1));
        return sglobal;
    }
    default: {
        INode *type = iexpGetTypeDcl(lval);
        if (type->tag == ArrayTag) {
            // LLVM provides no useful way to get the address of an array not stored in memory
            // For example, a literal array or an array returned by a function
            // So we hack it by storing in an unnamed local variable and return that address
            // This is particularly necessary when doing an array index ([1,2,5][n])  (LLVM fail at this too)
            LLVMValueRef temparray = genlAlloca(gen, genlType(gen, type), "temparray");
            LLVMBuildStore(gen->builder, genlExpr(gen, lval), temparray);
            return temparray;
        }
        assert(0 && "Cannot get address of this node");
        return NULL;
    }
    }
}

void genlStore(GenState *gen, INode *lval, LLVMValueRef rval) {
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;
    LLVMValueRef lvalptr = genlAddr(gen, lval);
    RefNode *reftype = (RefNode *)((IExpNode*)lval)->vtype;
    if (reftype->tag == RefTag && isRegion(reftype->region, rcName))
        genlRcCounter(gen, LLVMBuildLoad(gen->builder, lvalptr, "dealiasref"), -1, reftype);
    LLVMBuildStore(gen->builder, rval, lvalptr);
}

// Generate a term
LLVMValueRef genlExpr(GenState *gen, INode *termnode) {
    if (!gen->opt->release && gen->fn) {
        LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(gen->context, 
            termnode->linenbr, termnode->srcp-termnode->linep, LLVMGetSubprogram(gen->fn), NULL);
        LLVMValueRef val = LLVMMetadataAsValue(gen->context, loc);
        LLVMSetCurrentDebugLocation(gen->builder, val);
    }
    switch (termnode->tag) {
    case NilLitTag:
        return LLVMGetUndef(gen->emptyStructType);
    case ULitTag:
        return LLVMConstInt(genlType(gen, ((ULitNode*)termnode)->vtype), ((ULitNode*)termnode)->uintlit, 0);
    case FLitTag:
        return LLVMConstReal(genlType(gen, ((FLitNode*)termnode)->vtype), ((FLitNode*)termnode)->floatlit);
    case ArrayLitTag:
    {
        ArrayNode *lit = (ArrayNode *)termnode;
        uint32_t size = lit->elems->used;
        if (lit->dimens->used > 0) {
            // When array size specified for fill, use that
            INode *dimnode = nodesGet(lit->dimens, 0);
            while (dimnode->tag == VarNameUseTag)
                dimnode = ((ConstDclNode*)((NameUseNode*)dimnode)->dclnode)->value;
            assert(dimnode->tag == ULitTag);
            size = (uint32_t)((ULitNode*)dimnode)->uintlit;
        }
        LLVMValueRef *values = (LLVMValueRef *)memAllocBlk(size * sizeof(LLVMValueRef *));
        LLVMValueRef *valuep = values;
        if (lit->dimens->used > 0) {
            LLVMValueRef fillval = genlExpr(gen, nodesGet(lit->elems, 0));
            uint32_t cnt = size;
            while (cnt--)
                *valuep++ = fillval;
        }
        else {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(lit->elems, cnt, nodesp))
                *valuep++ = genlExpr(gen, *nodesp);
        }
        INode *elemtype = nodesGet(((ArrayNode *)itypeGetTypeDcl(lit->vtype))->elems, 0);
        return LLVMConstArray(genlType(gen, elemtype), values, size);
    }
    case TypeLitTag:
    {
        FnCallNode *lit = (FnCallNode *)termnode;
        INode *littype = itypeGetTypeDcl(lit->vtype);
        uint32_t size = lit->args->used;
        INode **nodesp;
        uint32_t cnt;
        if (littype->tag == StructTag) {
            if (littype->flags & NullablePtr) {
                // Special optimized logic for creating nullable ref/ptr value
                StructNode *strnode = (StructNode *)littype;
                LLVMTypeRef ptrtype = strnode->llvmtype;
                if (lit->args->used == 1) {
                    if (LLVMGetTypeKind(ptrtype) != LLVMPointerTypeKind) {
                        LLVMValueRef strval = LLVMGetUndef(ptrtype);
                        LLVMValueRef null = LLVMConstPointerNull(LLVMStructGetTypeAtIndex(ptrtype, 0));
                        return LLVMBuildInsertValue(gen->builder, strval, null, 0, "null");
                    }
                    return LLVMConstPointerNull(ptrtype);
                }
                else
                    return genlExpr(gen, nodesGet(lit->args, 1));
            }
            else {
                LLVMValueRef strval = LLVMGetUndef(genlType(gen, littype));
                unsigned int pos = 0;
                for (nodesFor(lit->args, cnt, nodesp))
                    strval = LLVMBuildInsertValue(gen->builder, strval, genlExpr(gen, *nodesp), pos++, "literal");
                return strval;
            }
        }
        else if (littype->tag == IntNbrTag || littype->tag == UintNbrTag || littype->tag == FloatNbrTag) {
            return genlConvert(gen, nodesGet(lit->args, 0), lit->objfn);
        }
        else {
            errorMsgNode((INode*)lit, ErrorBadTerm, "Unknown literal type to generate");
            return NULL;
        }
    }
    case NamedValTag:
        return genlExpr(gen, ((NamedValNode*)termnode)->val);
    case StringLitTag:
    {
        return LLVMBuildLoad(gen->builder, genlAddr(gen, termnode), "");
    }
    case VarNameUseTag:
    {
        VarDclNode *vardcl = (VarDclNode*)((NameUseNode *)termnode)->dclnode;
        if (vardcl->tag == VarDclTag)
            return LLVMBuildLoad(gen->builder, vardcl->llvmvar, &vardcl->namesym->namestr);
        else if (vardcl->tag == ConstDclTag) {
            ConstDclNode *constdcl = (ConstDclNode*)vardcl;
            return genlExpr(gen, constdcl->value);
        }
        else
            assert(0 && "Unknown DclNode");
    }
    case AliasTag:
    {
        AliasNode *anode = (AliasNode*)termnode;
        LLVMValueRef val = genlExpr(gen, anode->exp);
        RefNode *reftype = (RefNode*)iexpGetTypeDcl(termnode);
        if (reftype->tag == RefTag) {
            if (isRegion(reftype->region, soName))
                genlDealiasOwn(gen, val, reftype);
            else
                genlRcCounter(gen, val, anode->aliasamt, reftype);
        }
        else if (reftype->tag == TTupleTag) {
            TupleNode *tuple = (TupleNode*)reftype;
            INode **nodesp;
            uint32_t cnt;
            size_t index = 0;
            int16_t *countp = anode->counts;
            for (nodesFor(tuple->elems, cnt, nodesp)) {
                if (*countp != 0) {
                    reftype = (RefNode *)itypeGetTypeDcl(*nodesp);
                    LLVMValueRef strval = LLVMBuildExtractValue(gen->builder, val, index, "");
                    if (isRegion(reftype->region, soName))
                        genlDealiasOwn(gen, strval, reftype);
                    else
                        genlRcCounter(gen, strval, *countp, reftype);
                }
                ++index; ++countp;
            }
        }
        return val;
    }
    case FnCallTag:
        return genlFnCall(gen, (FnCallNode *)termnode);
    case ArrIndexTag:
    {
        // If no borrowing is involved, just get address of lval, then load value
        if (!(termnode->flags & FlagBorrow))
            return LLVMBuildLoad(gen->builder, genlAddr(gen, termnode), "");

        // If borrowing, alter fncall to shortcut around the borrow node
        FnCallNode *fncall = (FnCallNode *)termnode;
        assert(fncall->objfn->tag == BorrowTag);
        fncall->objfn = ((RefNode *)fncall->objfn)->vtexp;
        return genlAddr(gen, termnode);
    }
    case FldAccessTag:
    {
        FnCallNode *fncall = (FnCallNode *)termnode;
        if (fncall->methfld->tag == MbrNameUseTag) {
            FieldDclNode *flddcl = (FieldDclNode*)((NameUseNode*)fncall->methfld)->dclnode;
            INode *objtyp = iexpGetTypeDcl(fncall->objfn);
            if (objtyp->tag == VirtRefTag) {
                LLVMValueRef fldpRef = genlAddr(gen, termnode);
                return (termnode->flags & FlagBorrow) ? fldpRef : LLVMBuildLoad(gen->builder, fldpRef, "");
            }
            else if (termnode->flags & FlagBorrow) {
                return LLVMBuildStructGEP(gen->builder, genlAddr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
            }
            else {
                return LLVMBuildExtractValue(gen->builder, genlExpr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
            }
        }
        else if (fncall->methfld->tag == ULitTag) {
            ULitNode *ulit = (ULitNode*)fncall->methfld;
            if (termnode->flags & FlagBorrow) {
                return LLVMBuildStructGEP(gen->builder, genlAddr(gen, fncall->objfn), (unsigned int)ulit->uintlit, "");
            }
            else {
                return LLVMBuildExtractValue(gen->builder, genlExpr(gen, fncall->objfn), (unsigned int)ulit->uintlit, "");
            }
        }
        else
            assert(0 && "Invalid FldAccess methfld.");
    }
    case SwapTag:
    {
        SwapNode *node = (SwapNode*)termnode;
        INode *lval = node->lval;
        INode *rval = node->rval;
        LLVMValueRef lvalptr = genlAddr(gen, lval);
        LLVMValueRef rvalptr = genlAddr(gen, rval);
        LLVMValueRef rightval = LLVMBuildLoad(gen->builder, rvalptr, "");
        LLVMValueRef leftval = LLVMBuildLoad(gen->builder, lvalptr, "");
        LLVMBuildStore(gen->builder, rightval, lvalptr);
        LLVMBuildStore(gen->builder, leftval, rvalptr);
        return leftval;
    }
    case AssignTag:
    {
        AssignNode *node = (AssignNode*)termnode;
        INode *lval = node->lval;
        INode *rval = node->rval;
        LLVMValueRef valueref = genlExpr(gen, rval);
        if (node->assignType == LeftAssign) {
            // Normal assignment, except value of expression is contents of lval before mutation
            LLVMValueRef lvalptr = genlAddr(gen, lval);
            LLVMValueRef leftval = LLVMBuildLoad(gen->builder, lvalptr, "");
            LLVMBuildStore(gen->builder, valueref, lvalptr);
            return leftval;
        }

        if (lval->tag != VTupleTag) {
            if (rval->tag != VTupleTag) {
                genlStore(gen, lval, valueref); // simple assignment
            }
            else {
                // Assign lval to first value in tuple struct
                genlStore(gen, lval, LLVMBuildExtractValue(gen->builder, valueref, 0, ""));
            }
        }
        else {
            // Parallel assignment: each lval to value in tuple struct
            TupleNode *ltuple = (TupleNode *)lval;
            INode **nodesp;
            uint32_t cnt;
            int index = 0;
            for (nodesFor(ltuple->elems, cnt, nodesp)) {
                genlStore(gen, *nodesp, LLVMBuildExtractValue(gen->builder, valueref, index++, ""));
            }
        }
        return valueref;
    }
    case VTupleTag:
    {
        // Load only: Creates an ad hoc struct to hold the tuple's values
        TupleNode *tuple = (TupleNode *)termnode;
        LLVMValueRef tupleval = LLVMGetUndef(genlType(gen, tuple->vtype));
        INode **nodesp;
        uint32_t cnt;
        unsigned int pos = 0;
        for (nodesFor(tuple->elems, cnt, nodesp)) {
            tupleval = LLVMBuildInsertValue(gen->builder, tupleval, genlExpr(gen, *nodesp), pos++, "tuple");
        }
        return tupleval;
    }
    case SizeofTag:
        return genlSizeof(gen, ((SizeofNode*)termnode)->type);
    case CastTag:
    {
        CastNode *node = (CastNode*)termnode;
        if (node->flags & FlagConvert)
            return genlConvert(gen, node->exp, node->vtype);
        return genlRecast(gen, node->exp, node->vtype);
    }
    case IsTag:
    {
        return genlIsType(gen, (CastNode *)termnode);
    }
    case BorrowTag:
    {
        RefNode *anode = (RefNode*)termnode;
        return genlAddr(gen, anode->vtexp);
    }
    case ArrayBorrowTag:
    {
        RefNode *anode = (RefNode*)termnode;
        RefNode *reftype = (RefNode *)anode->vtype;
        INode *arraytype = itypeGetTypeDcl(((IExpNode*)anode->vtexp)->vtype);
        if (arraytype->tag == ArrayTag) {
            LLVMValueRef tupleval = LLVMGetUndef(genlType(gen, (INode*)reftype));
            LLVMTypeRef ptrtype = LLVMPointerType(genlType(gen, reftype->vtexp), 0);
            LLVMValueRef ptr = LLVMBuildBitCast(gen->builder, genlAddr(gen, anode->vtexp), ptrtype, "fatptr");
            tupleval = LLVMBuildInsertValue(gen->builder, tupleval, ptr, 0, "fatptr");
            LLVMValueRef size = LLVMConstInt(genlType(gen, (INode*)usizeType), arrayDim1(arraytype), 0);
            return LLVMBuildInsertValue(gen->builder, tupleval, size, 1, "fatsize");
        }
        else if (arraytype->tag == ArrayDerefTag) {
            return genlExpr(gen, ((StarNode*)anode->vtexp)->vtexp);
        }
        else {
            // Build fat pointer slice to a single element
            LLVMValueRef tupleval = LLVMGetUndef(genlType(gen, (INode*)reftype));
            tupleval = LLVMBuildInsertValue(gen->builder, tupleval, genlAddr(gen, anode->vtexp), 0, "fatptr");
            LLVMValueRef size = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
            return LLVMBuildInsertValue(gen->builder, tupleval, size, 1, "fatsize");
        }
    }
    case AllocateTag:
    case ArrayAllocTag:
        return genlallocref(gen, (RefNode*)termnode);
    case DerefTag:
        return LLVMBuildLoad(gen->builder, genlExpr(gen, ((StarNode*)termnode)->vtexp), "deref");
    case OrLogicTag: case AndLogicTag:
        return genlLogic(gen, (LogicNode*)termnode);
    case NotLogicTag:
        return genlNot(gen, (LogicNode*)termnode);
    case VarDclTag:
        return genlLocalVar(gen, (VarDclNode*)termnode); break;
    case BlockTag:
        return genlBlock(gen, (BlockNode*)termnode); break;
    case IfTag:
        return genlIf(gen, (IfNode*)termnode); break;
    default:
        assert(0 && "Unknown node to genlExpr!");
        return NULL;
    }
}
