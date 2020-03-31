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
    LLVMBasicBlockRef nextMemBlk;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        // Set up block for next condition (or endif if this is last condition)
        if (i + 1 < count)
            nextif = nextMemBlk = LLVMInsertBasicBlockInContext(gen->context, endif, "ifnext");
        else
            nextif = endif;

        // Set up this condition's statement block and then conditionally jump to it or next condition
        LLVMBasicBlockRef ablk;
        if (*nodesp != elseCond) {
            ablk = LLVMInsertBasicBlockInContext(gen->context, nextif, "ifblk");
            LLVMBuildCondBr(gen->builder, genlExpr(gen, *nodesp), ablk, nextif);
            LLVMPositionBuilderAtEnd(gen->builder, ablk);
        }
        else
            ablk = nextMemBlk;

        // Generate this condition's code block, along with jump to endif if block does not end with a return
        LLVMValueRef blkval = genlBlock(gen, (BlockNode*)*(nodesp + 1));
        uint16_t lastStmttype = nodesLast(((BlockNode*)*(nodesp + 1))->stmts)->tag;
        if (lastStmttype != ReturnTag && lastStmttype != BreakTag && lastStmttype != ContinueTag) {
            LLVMBuildBr(gen->builder, endif);
            // Remember value and block if needed for phi merge
            if (vtype != unknownType) {
                blkvals[phicnt] = blkval;
                blks[phicnt++] = ablk;
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

// Generate a function call, including special intrinsics
LLVMValueRef genlFnCall(GenState *gen, FnCallNode *fncall) {

    // Get Valuerefs for all the parameters to pass to the function
    LLVMValueRef fncallret = NULL;
    LLVMValueRef *fnargs = (LLVMValueRef*)memAllocBlk(fncall->args->used * sizeof(LLVMValueRef*));
    LLVMValueRef *fnarg = fnargs;
    INode **nodesp;
    uint32_t cnt;
    int vdispatch = fncall->flags & FlagVDisp;
    LLVMValueRef vref;
    for (nodesFor(fncall->args, cnt, nodesp)) {
        // For virtual reference, extract the object's regular reference
        if (vdispatch) {
            vdispatch = 0;
            vref = genlExpr(gen, *nodesp);
            *fnarg++ = LLVMBuildExtractValue(gen->builder, vref, 0, "");
        }
        else
            *fnarg++ = genlExpr(gen, *nodesp);
    }

    // Handle call when we have a derefed pointer to a function
    if (fncall->objfn->tag == DerefTag) {
        return LLVMBuildCall(gen->builder, genlExpr(gen, ((DerefNode*)fncall->objfn)->exp), fnargs, fncall->args->used, "");
    }
    // Handle call when we have a ref or pointer to a function
    INode *fntype = iexpGetTypeDcl(fncall->objfn);
    if (fntype->tag != FnSigTag) {
        assert(fntype->tag == RefTag || fntype->tag == PtrTag);
        return LLVMBuildCall(gen->builder, genlExpr(gen, fncall->objfn), fnargs, fncall->args->used, "");
    }
    // We know at this point that fncall->objfn refers to some "named" function
    // Handle call when first argument (object) is a virtual reference
    if (fncall->flags & FlagVDisp) {
        FnDclNode *methdcl = (FnDclNode*)((NameUseNode *)fncall->objfn)->dclnode;
        LLVMValueRef vtable = LLVMBuildExtractValue(gen->builder, vref, 1, "");
        LLVMValueRef vtblmethp = LLVMBuildStructGEP(gen->builder, vtable, methdcl->vtblidx, &methdcl->namesym->namestr); // **fn
        LLVMValueRef vtblmeth = LLVMBuildLoad(gen->builder, vtblmethp, "");
        return LLVMBuildCall(gen->builder, vtblmeth, fnargs, fncall->args->used, "");
    }

    // A function call may be to an intrinsic, or to program-defined code
    NameUseNode *fnuse = (NameUseNode *)fncall->objfn;
    FnDclNode *fndcl = (FnDclNode *)fnuse->dclnode;
    switch (fndcl->value? fndcl->value->tag : BlockTag) {
    case BlockTag: {
        fncallret = LLVMBuildCall(gen->builder, fndcl->llvmvar, fnargs, fncall->args->used, "");
        if (fndcl->flags & FlagSystem) {
            LLVMSetInstructionCallConv(fncallret, LLVMX86StdcallCallConv);
        }
        break;
    }
    case IntrinsicTag: {
        NbrNode *nbrtype = (NbrNode *)iexpGetTypeDcl(*nodesNodes(fncall->args));
        uint16_t typetag = nbrtype->tag;

        // Pointer intrinsics
        if (typetag == PtrTag || typetag == RefTag) {
            INode *pvtype = itypeGetTypeDcl(typetag == PtrTag ? ((PtrNode*)nbrtype)->pvtype : ((RefNode*)nbrtype)->pvtype);
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
                // Comparison
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            case LtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], ""); break;
            case LeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], ""); break;
            case GtIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], ""); break;
            case GeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], ""); break;
            case IncrIntrinsic: // ++x
            {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMTypeRef selftype = genlType(gen, pvtype);
                if (pvtype->tag == PtrTag || pvtype->tag == RefTag) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
                    fncallret = LLVMBuildGEP(gen->builder, val, &constone, 1, "");
                }
                else if (pvtype->tag == FloatNbrTag)
                    fncallret = LLVMBuildFAdd(gen->builder, val, LLVMConstReal(selftype, 1.), "");
                else
                    fncallret = LLVMBuildAdd(gen->builder, val, LLVMConstInt(selftype, 1, 0), "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;
            }
            case DecrIntrinsic: // --x
            {
                LLVMValueRef val = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMTypeRef selftype = genlType(gen, pvtype);
                if (pvtype->tag == PtrTag || pvtype->tag == RefTag) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), -1, 1);
                    fncallret = LLVMBuildGEP(gen->builder, val, &constone, 1, "");
                }
                else if (pvtype->tag == FloatNbrTag)
                    fncallret = LLVMBuildFSub(gen->builder, val, LLVMConstReal(selftype, 1.), "");
                else
                    fncallret = LLVMBuildSub(gen->builder, val, LLVMConstInt(selftype, 1, 0), "");
                LLVMBuildStore(gen->builder, fncallret, fnargs[0]);
                break;
            }
            case IncrPostIntrinsic: // x++
            {
                fncallret = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMTypeRef selftype = genlType(gen, pvtype);
                LLVMValueRef val;
                if (pvtype->tag == PtrTag || pvtype->tag == RefTag) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
                    val = LLVMBuildGEP(gen->builder, fncallret, &constone, 1, "");
                }
                else if (pvtype->tag == FloatNbrTag)
                    val = LLVMBuildFAdd(gen->builder, fncallret, LLVMConstReal(selftype, 1.), "");
                else
                    val = LLVMBuildAdd(gen->builder, fncallret, LLVMConstInt(selftype, 1, 0), "");
                LLVMBuildStore(gen->builder, val, fnargs[0]);
                break;
            }
            case DecrPostIntrinsic: // x--
            {
                fncallret = LLVMBuildLoad(gen->builder, fnargs[0], "");
                LLVMTypeRef selftype = genlType(gen, pvtype);
                LLVMValueRef val;
                if (pvtype->tag == PtrTag || pvtype->tag == RefTag) {
                    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), -1, 1);
                    val = LLVMBuildGEP(gen->builder, fncallret, &constone, 1, "");
                }
                else if (pvtype->tag == FloatNbrTag)
                    val = LLVMBuildFSub(gen->builder, fncallret, LLVMConstReal(selftype, 1.), "");
                else
                    val = LLVMBuildSub(gen->builder, fncallret, LLVMConstInt(selftype, 1, 0), "");
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
                long long valsize = LLVMABISizeOfType(gen->datalayout, genlType(gen, ((PtrNode*)nbrtype)->pvtype));
                fncallret = LLVMBuildSDiv(gen->builder, diff, LLVMConstInt(genlType(gen, (INode*)usizeType), valsize, 1), "");
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

        // Array reference intrinsics
        else if (typetag == ArrayRefTag) {
            INode *pvtype = itypeGetTypeDcl(typetag == PtrTag ? ((PtrNode*)nbrtype)->pvtype : ((RefNode*)nbrtype)->pvtype);
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
            case CountIntrinsic: fncallret = LLVMBuildExtractValue(gen->builder, fnargs[0], 1, "slicecount"); break;
            // Comparison
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            }
        }

        // Floating point intrinsics
        else if (typetag == FloatNbrTag) {
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
            case NegIntrinsic: fncallret = LLVMBuildFNeg(gen->builder, fnargs[0], ""); break;
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
                char *fnname = nbrtype->bits == 32 ? "llvm.sqrt.f32" : "llvm.sqrt.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fncall->args->used, "");
                break;
            }
            case SinIntrinsic:
            {
                char *fnname = nbrtype->bits == 32 ? "llvm.sin.f32" : "llvm.sin.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fncall->args->used, "");
                break;
            }
            case CosIntrinsic:
            {
                char *fnname = nbrtype->bits == 32 ? "llvm.cos.f32" : "llvm.cos.f64";
                fncallret = LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fncall->args->used, "");
                break;
            }
            }
        }
        // Signed and Unsigned Integer intrinsics
        else {
            switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {

                // Arithmetic
            case NegIntrinsic: fncallret = LLVMBuildNeg(gen->builder, fnargs[0], ""); break;
            case AddIntrinsic: fncallret = LLVMBuildAdd(gen->builder, fnargs[0], fnargs[1], ""); break;
            case SubIntrinsic: fncallret = LLVMBuildSub(gen->builder, fnargs[0], fnargs[1], ""); break;
            case MulIntrinsic: fncallret = LLVMBuildMul(gen->builder, fnargs[0], fnargs[1], ""); break;
            case DivIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildSDiv(gen->builder, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildUDiv(gen->builder, fnargs[0], fnargs[1], ""); break;
                }
            case RemIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildSRem(gen->builder, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildURem(gen->builder, fnargs[0], fnargs[1], ""); break;
                }

                // Comparison
            case EqIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], ""); break;
            case NeIntrinsic: fncallret = LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], ""); break;
            case LtIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntSLT, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], ""); break;
                }
            case LeIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntSLE, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], ""); break;
                }
            case GtIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntSGT, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], ""); break;
                }
            case GeIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntSGE, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], ""); break;
                }

            // Bitwise
            case NotIntrinsic: fncallret = LLVMBuildNot(gen->builder, fnargs[0], ""); break;
            case AndIntrinsic: fncallret = LLVMBuildAnd(gen->builder, fnargs[0], fnargs[1], ""); break;
            case OrIntrinsic: fncallret = LLVMBuildOr(gen->builder, fnargs[0], fnargs[1], ""); break;
            case XorIntrinsic: fncallret = LLVMBuildXor(gen->builder, fnargs[0], fnargs[1], ""); break;
            case ShlIntrinsic: fncallret = LLVMBuildShl(gen->builder, fnargs[0], fnargs[1], ""); break;
            case ShrIntrinsic:
                if (typetag == IntNbrTag) {
                    fncallret = LLVMBuildAShr(gen->builder, fnargs[0], fnargs[1], ""); break;
                }
                else {
                    fncallret = LLVMBuildLShr(gen->builder, fnargs[0], fnargs[1], ""); break;
                }
            }
        }
        break;
    }
    default:
        assert(0 && "invalid type of function call");
    }

    return fncallret;
}

// Generate a value converted to another type
// - Numbers to another number (extending or truncating)
// - Number or ref/ptr to true/false
// - Array ref to ptr or size
LLVMValueRef genlConvert(GenState *gen, INode* exp, INode* to) {
    INode *fromtype = iexpGetTypeDcl(exp);
    INode *totype = itypeGetTypeDcl(to);
    LLVMValueRef genexp = genlExpr(gen, exp);

    // Casting a number to Bool means false if zero and true otherwise
    if (totype == (INode*)boolType) {
        if (fromtype->tag == RefTag || fromtype->tag == PtrTag)
            return LLVMBuildIsNotNull(gen->builder, genexp, "isnotnull");
        else if (fromtype->tag == FloatNbrTag)
            return LLVMBuildFCmp(gen->builder, LLVMRealONE, genexp, LLVMConstNull(genlType(gen, fromtype)), "");
        else
            return LLVMBuildICmp(gen->builder, LLVMIntNE, genexp, LLVMConstNull(genlType(gen, fromtype)), "");
    }

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
            ArrayNode *arraytype = (ArrayNode*)((RefNode *)fromtype)->pvtype;
            assert(arraytype->tag == ArrayTag);
            LLVMValueRef aref = LLVMGetUndef(genlType(gen, totype));
            LLVMValueRef size = LLVMConstInt(genlUsize(gen), arraytype->size, 0);
            aref = LLVMBuildInsertValue(gen->builder, aref, genexp, 0, "arrayp");
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
        StructNode *trait = (StructNode*)itypeGetTypeDcl(((RefNode*)totype)->pvtype);
        StructNode *strnode = (StructNode*)itypeGetTypeDcl(((RefNode*)fromtype)->pvtype);
        Vtable *vtable = ((StructNode*)trait)->vtable;
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
    LLVMBasicBlockRef logicblks[2], logicphi;
    LLVMValueRef logicvals[2];

    // Set up basic blocks
    logicblks[0] = LLVMGetInsertBlock(gen->builder);
    logicphi = genlInsertBlock(gen, node->tag==AndLogicTag? "andphi" : "orphi");
    logicblks[1] = genlInsertBlock(gen, node->tag==AndLogicTag? "andrhs" : "orrhs");

    // Generate left-hand condition and conditional branch
    logicvals[0] = genlExpr(gen, node->lexp);
    if (node->tag==OrLogicTag)
        LLVMBuildCondBr(gen->builder, logicvals[0], logicphi, logicblks[1]);
    else
        LLVMBuildCondBr(gen->builder, logicvals[0], logicblks[1], logicphi);

    // Generate right-hand condition and branch to phi
    LLVMPositionBuilderAtEnd(gen->builder, logicblks[1]);
    logicvals[1] = genlExpr(gen, node->rexp);
    LLVMBuildBr(gen->builder, logicphi);

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
    StructNode *structtype = (StructNode*)(istype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)istype)->pvtype) : istype);

    // If pattern matching a virtual reference, compare vtable pointers
    if (exptype->tag == VirtRefTag) {
        LLVMValueRef vtablep = LLVMBuildExtractValue(gen->builder, val, 1, "");
        Vtable *vtable = structGetBaseTrait(structtype)->vtable;
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
        return genlExpr(gen, ((DerefNode *)lval)->exp);
    case ArrIndexTag:
    {
        FnCallNode *fncall = (FnCallNode *)lval;
        INode *objtype = iexpGetTypeDcl(fncall->objfn);
        switch (objtype->tag) {
        case ArrayTag: {
            LLVMValueRef count = LLVMConstInt(genlUsize(gen), ((ArrayNode*)objtype)->size, 0);
            LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
            genlBoundsCheck(gen, index, count);
            LLVMValueRef indexes[2];
            indexes[0] = LLVMConstInt(genlUsize(gen), 0, 0);
            indexes[1] = index;
            return LLVMBuildGEP(gen->builder, genlAddr(gen, fncall->objfn), indexes, 2, "");
        }
        case ArrayRefTag: {
            LLVMValueRef arrref = genlExpr(gen, fncall->objfn);
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
    case StringLitTag:
    {
        SLitNode *strnode = (SLitNode *)lval;
        ArrayNode *anode = (ArrayNode*)strnode->vtype;
        LLVMValueRef sglobal = LLVMAddGlobal(gen->module, genlType(gen, strnode->vtype), "string");
        LLVMSetLinkage(sglobal, LLVMInternalLinkage);
        LLVMSetGlobalConstant(sglobal, 1);
        LLVMSetInitializer(sglobal, LLVMConstStringInContext(gen->context, strnode->strlit, anode->size, 1));
        return sglobal;
    }
    }
    return NULL;
}

void genlStore(GenState *gen, INode *lval, LLVMValueRef rval) {
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;
    LLVMValueRef lvalptr = genlAddr(gen, lval);
    RefNode *reftype = (RefNode *)((IExpNode*)lval)->vtype;
    if (reftype->tag == RefTag && reftype->region == (INode*)rcRegion)
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
    case ULitTag:
        return LLVMConstInt(genlType(gen, ((ULitNode*)termnode)->vtype), ((ULitNode*)termnode)->uintlit, 0);
    case FLitTag:
        return LLVMConstReal(genlType(gen, ((FLitNode*)termnode)->vtype), ((FLitNode*)termnode)->floatlit);
    case NullTag:
    {
        INode *ptrtype = ((ULitNode*)termnode)->vtype;
        return LLVMConstNull(ptrtype? genlType(gen, ptrtype) : LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0));
    }
    case TypeLitTag:
    {
        FnCallNode *lit = (FnCallNode *)termnode;
        INode *littype = itypeGetTypeDcl(lit->vtype);
        uint32_t size = lit->args->used;
        INode **nodesp;
        uint32_t cnt;
        if (littype->tag == ArrayTag) {
            LLVMValueRef *values = (LLVMValueRef *)memAllocBlk(size * sizeof(LLVMValueRef *));
            LLVMValueRef *valuep = values;
            for (nodesFor(lit->args, cnt, nodesp))
                *valuep++ = genlExpr(gen, *nodesp);
            return LLVMConstArray(genlType(gen, ((ArrayNode *)lit->vtype)->elemtype), values, size);
        }
        else if (littype->tag == StructTag) {
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
        return LLVMBuildLoad(gen->builder, vardcl->llvmvar, &vardcl->namesym->namestr);
    }
    case AliasTag:
    {
        AliasNode *anode = (AliasNode*)termnode;
        LLVMValueRef val = genlExpr(gen, anode->exp);
        RefNode *reftype = (RefNode*)iexpGetTypeDcl(termnode);
        if (reftype->tag == RefTag) {
            if (reftype->region == (INode*)soRegion)
                genlDealiasOwn(gen, val, reftype);
            else
                genlRcCounter(gen, val, anode->aliasamt, reftype);
        }
        else if (reftype->tag == TTupleTag) {
            TTupleNode *tuple = (TTupleNode*)reftype;
            INode **nodesp;
            uint32_t cnt;
            size_t index = 0;
            int16_t *countp = anode->counts;
            for (nodesFor(tuple->types, cnt, nodesp)) {
                if (*countp != 0) {
                    reftype = (RefNode *)itypeGetTypeDcl(*nodesp);
                    LLVMValueRef strval = LLVMBuildExtractValue(gen->builder, val, index, "");
                    if (reftype->region == (INode*)soRegion)
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
        if (termnode->flags & FlagBorrow) {
            FnCallNode *fncall = (FnCallNode *)termnode;
            INode *objtype = iexpGetTypeDcl(fncall->objfn);
            switch (objtype->tag) {
            case ArrayTag: {
                LLVMValueRef count = LLVMConstInt(genlUsize(gen), ((ArrayNode*)objtype)->size, 0);
                LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
                genlBoundsCheck(gen, index, count);
                LLVMValueRef indexes[2];
                indexes[0] = LLVMConstInt(genlUsize(gen), 0, 0);
                indexes[1] = index;
                return LLVMBuildGEP(gen->builder, genlAddr(gen, fncall->objfn), indexes, 2, "");
            }
            case ArrayDerefTag: {
                DerefNode *deref = (DerefNode *)fncall->objfn;
                assert(deref->tag == DerefTag);
                LLVMValueRef arrref = genlExpr(gen, deref->exp);
                LLVMValueRef count = LLVMBuildExtractValue(gen->builder, arrref, 1, "count");
                LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
                genlBoundsCheck(gen, index, count);
                LLVMValueRef sliceptr = LLVMBuildExtractValue(gen->builder, arrref, 0, "sliceptr");
                return LLVMBuildGEP(gen->builder, sliceptr, &index, 1, "");
            }
            case ArrayRefTag: {
                LLVMValueRef arrref = genlExpr(gen, fncall->objfn);
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
        else
            return LLVMBuildLoad(gen->builder, genlAddr(gen, termnode), "");
    case FldAccessTag:
    {
        FnCallNode *fncall = (FnCallNode *)termnode;
        FieldDclNode *flddcl = (FieldDclNode*)((NameUseNode*)fncall->methfld)->dclnode;
        INode *objtyp = iexpGetTypeDcl(fncall->objfn);
        if (objtyp->tag == VirtRefTag) {
            LLVMValueRef fldpRef = genlAddr(gen, termnode);
            return (termnode->flags & FlagBorrow)? fldpRef : LLVMBuildLoad(gen->builder, fldpRef, "");
        }
        else if (termnode->flags & FlagBorrow) {
            return LLVMBuildStructGEP(gen->builder, genlAddr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
        }
        else {
            return LLVMBuildExtractValue(gen->builder, genlExpr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
        }
    }
    case AssignTag:
    {
        AssignNode *node = (AssignNode*)termnode;
        INode *lval = node->lval;
        INode *rval = node->rval;
        LLVMValueRef valueref = genlExpr(gen, rval);
        if (lval->tag != VTupleTag) {
            if (rval->tag != VTupleTag)
                genlStore(gen, lval, valueref); // simple assignment
            else {
                // Assign lval to first value in tuple struct
                genlStore(gen, lval, LLVMBuildExtractValue(gen->builder, valueref, 0, ""));
            }
        }
        else {
            // Parallel assignment: each lval to value in tuple struct
            VTupleNode *ltuple = (VTupleNode *)lval;
            INode **nodesp;
            uint32_t cnt;
            int index = 0;
            for (nodesFor(ltuple->values, cnt, nodesp)) {
                genlStore(gen, *nodesp, LLVMBuildExtractValue(gen->builder, valueref, index++, ""));
            }
        }
        return valueref;
    }
    case VTupleTag:
    {
        // Load only: Creates an ad hoc struct to hold the tuple's values
        VTupleNode *tuple = (VTupleNode *)termnode;
        LLVMValueRef tupleval = LLVMGetUndef(genlType(gen, tuple->vtype));
        INode **nodesp;
        uint32_t cnt;
        unsigned int pos = 0;
        for (nodesFor(tuple->values, cnt, nodesp)) {
            tupleval = LLVMBuildInsertValue(gen->builder, tupleval, genlExpr(gen, *nodesp), pos++, "tuple");
        }
        return tupleval;
    }
    case SizeofTag:
        return genlSizeof(gen, ((SizeofNode*)termnode)->type);
    case CastTag:
    {
        CastNode *node = (CastNode*)termnode;
        if (node->flags & FlagRecast)
            return genlRecast(gen, node->exp, node->vtype);
        return genlConvert(gen, node->exp, node->vtype);
    }
    case IsTag:
    {
        return genlIsType(gen, (CastNode *)termnode);
    }
    case BorrowTag:
    {
        BorrowNode *anode = (BorrowNode*)termnode;
        RefNode *reftype = (RefNode *)anode->vtype;
        if (reftype->tag == ArrayRefTag) {
            INode *arraytype = itypeGetTypeDcl(((IExpNode*)anode->exp)->vtype);
            if (arraytype->tag == ArrayTag) {
                LLVMValueRef tupleval = LLVMGetUndef(genlType(gen, (INode*)reftype));
                LLVMTypeRef ptrtype = LLVMPointerType(genlType(gen, reftype->pvtype), 0);
                LLVMValueRef ptr = LLVMBuildBitCast(gen->builder, genlAddr(gen, anode->exp), ptrtype, "fatptr");
                tupleval = LLVMBuildInsertValue(gen->builder, tupleval, ptr, 0, "fatptr");
                LLVMValueRef size = LLVMConstInt(genlType(gen, (INode*)usizeType), ((ArrayNode*)arraytype)->size, 0);
                return LLVMBuildInsertValue(gen->builder, tupleval, size, 1, "fatsize");
            }
            else if (arraytype->tag == ArrayDerefTag) {
                return genlExpr(gen, ((DerefNode*)anode->exp)->exp);
            }
        }
        else
            return genlAddr(gen, anode->exp);
    }
    case AllocateTag:
        return genlallocref(gen, (AllocateNode*)termnode);
    case DerefTag:
        return LLVMBuildLoad(gen->builder, genlExpr(gen, ((DerefNode*)termnode)->exp), "deref");
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
    case LoopTag:
        return genlLoop(gen, (LoopNode*)termnode); break;
    default:
        assert(0 && "Unknown node to genlExpr!");
        return NULL;
    }
}
