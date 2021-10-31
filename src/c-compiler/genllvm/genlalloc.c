/** Allocator generation via LLVM
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

// Build usable metadata about a reference 
void genlRefTypeSetup(GenState *gen, RefNode *reftype) {
    if (reftype->region->tag == BorrowRegTag)
        return;

    RefTypeInfo *refinfo = reftype->typeinfo;

    // Build composite struct, with "fields" for region, perm, and vtype
    LLVMTypeRef field_types[3];
    LLVMTypeRef *fieldtypep = &field_types[0];
    *fieldtypep++ = genlType(gen, reftype->region);
    *fieldtypep++ = genlType(gen, reftype->perm);
    *fieldtypep = genlType(gen, reftype->vtexp);
    LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, "refstruct");
    LLVMStructSetBody(structype, field_types, 3, 0);
    refinfo->structype = structype;

    refinfo->ptrstructype = LLVMPointerType(structype, 0);
}


// Function declarations for malloc() and free()
LLVMValueRef genlfreeval = NULL;

// If ref type is struct, dealias any fields holding rc/own references
void genlDealiasFlds(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    StructNode *strnode = (StructNode*)itypeGetTypeDcl(refnode->vtexp);
    if (strnode->tag != StructTag)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        RefNode *vartype = (RefNode *)field->vtype;
        if (vartype->tag != RefTag || !(isRegion(vartype->region, rcName) || isRegion(vartype->region, soName)))
            continue;
        LLVMValueRef fldref = LLVMBuildStructGEP(gen->builder, ref, field->index, &field->namesym->namestr);
        if (isRegion(vartype->region, soName))
            genlDealiasOwn(gen, fldref, vartype);
        else
            genlRcCounter(gen, fldref, -1, vartype);
    }
}

// Call free() (and generate declaration if needed)
LLVMValueRef genlFree(GenState *gen, LLVMValueRef ref) {
    LLVMTypeRef parmtype = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
    // Declare free() external function
    if (genlfreeval == NULL) {
        LLVMTypeRef rettype = LLVMVoidTypeInContext(gen->context);
        LLVMTypeRef fnsig = LLVMFunctionType(rettype, &parmtype, 1, 0);
        genlfreeval = LLVMAddFunction(gen->module, "free", fnsig);
    }
    // Cast ref to *u8 and then call free()
    LLVMValueRef refcast = LLVMBuildBitCast(gen->builder, ref, parmtype, "");
    return LLVMBuildCall(gen->builder, genlfreeval, &refcast, 1, "");
}

// Generate region-based allocation and initialization logc
// It returns a reference to the allocated/initialized object (or null)
// This is roughly what it does:
//
// fn allocate(size usize) +region-uni T
//   imm ref = region::_alloc(T.size) as +region-uni T
//   if (ref is None)
//     panic or return None
//   ref.region.init()
//   ref.perm.init()
//   T::init(&mut ref.TValue, initvalue)
//   &ref.TValue or Some[&ref.TValue]
//
LLVMValueRef genlallocref(GenState *gen, RefNode *allocatenode) {
    RefNode *reftype = (RefNode*)itypeGetTypeDcl(allocatenode->vtype);
    if (reftype->tag != RefTag) {
        // Extract reftype from Option type
        assert(reftype->tag == StructTag && (allocatenode->flags & FlagQues) && "Should be Option type");
        StructNode *optionTrait = (StructNode*)reftype;
        StructNode *someStruct = (StructNode*)nodesGet(optionTrait->derived, 1);
        reftype = (RefNode*)itypeGetTypeDcl(((IExpNode*)nodelistGet(&someStruct->fields, 0))->vtype);
        assert(reftype->tag == RefTag && "Option type did not have reftype");
    }
    INode *region = itypeGetTypeDcl(reftype->region);
    INode *perm = itypeGetTypeDcl(reftype->perm);
    LLVMTypeRef valueptrtyp = LLVMPointerType(LLVMStructGetTypeAtIndex(LLVMGetElementType(reftype->typeinfo->ptrstructype), 2), 0);

    // Calculate how much memory space we need to allocate
    long long allocsize = LLVMABISizeOfType(gen->datalayout, reftype->typeinfo->structype);
    // For array-refs: Increase allocsz by (elemsz-1) * valuetype-size
    LLVMValueRef sizeval = LLVMConstInt(genlType(gen, (INode*)usizeType), allocsize, 0);

    // Do region allocation (using its _alloc method) and then bitcast to multi-layered-struct ptr
    FnDclNode *allocmeth = (FnDclNode*)iTypeFindFnField(region, allocMethodName);
    LLVMValueRef malloc = genlFnCallInternal(gen, SimpleDispatch, (INode*)allocmeth, 1, &sizeval);
    LLVMValueRef ptrstructype = LLVMBuildBitCast(gen->builder, malloc, reftype->typeinfo->ptrstructype, "");

    // Handle when allocation fails (returns NULL pointer)
    LLVMValueRef isNull = LLVMBuildIsNull(gen->builder, ptrstructype, "isnull");
    LLVMBasicBlockRef endif = genlInsertBlock(gen, "endif");
    LLVMBasicBlockRef initblk = genlInsertBlock(gen, "initblk");
    LLVMValueRef blkvals[2];
    LLVMBasicBlockRef blks[2];

    // Generate null/panic block, used when alloc returns 0
    LLVMBasicBlockRef panicblk = genlInsertBlock(gen, "panicblk");
    LLVMBuildCondBr(gen->builder, isNull, panicblk, initblk);
    LLVMPositionBuilderAtEnd(gen->builder, panicblk);
    if (!(allocatenode->flags & FlagQues))
        genlPanic(gen);    
    blkvals[0] = LLVMBuildBitCast(gen->builder, ptrstructype, valueptrtyp, "");
    blks[0] = panicblk;
    LLVMBuildBr(gen->builder, endif);
    LLVMPositionBuilderAtEnd(gen->builder, initblk);

    // Initialize region using its 'init' method, if supplied
    INode *reginitmeth = iTypeFindFnField(region, initMethodName);
    if (reginitmeth) {
        LLVMValueRef regionp = LLVMBuildStructGEP(gen->builder, ptrstructype, 0, "region");
        genlFnCallInternal(gen, SimpleDispatch, (INode*)reginitmeth, 1, &regionp);
    }

    // Initialize permission, if it is a locked permission with an init method
    if (perm->tag == StructTag) {
        INode *perminitmeth = iTypeFindFnField(perm, initMethodName);
        if (perminitmeth) {
            LLVMValueRef permp = LLVMBuildStructGEP(gen->builder, ptrstructype, 1, "perm");
            genlFnCallInternal(gen, SimpleDispatch, (INode*)perminitmeth, 1, &permp);
        }
    }

    // Initialize value (via copy or init function) and return pointer to it
    LLVMValueRef valuep = LLVMBuildStructGEP(gen->builder, ptrstructype, ValueField, ""); // Point to value
    LLVMBuildStore(gen->builder, genlExpr(gen, allocatenode->vtexp), valuep); // Copy value
    blkvals[1] = valuep;
    blks[1] = initblk;

    // Finish up block, start new one, and return allocated
    LLVMBuildBr(gen->builder, endif);
    LLVMPositionBuilderAtEnd(gen->builder, endif);
    LLVMValueRef phi = LLVMBuildPhi(gen->builder, valueptrtyp, "allocphi");
    LLVMAddIncoming(phi, blkvals, blks, 2);
    return phi;
}

// Dealias an own allocated reference
void genlDealiasOwn(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    genlDealiasFlds(gen, ref, refnode);
    genlFree(gen, ref);
}

// Add to the counter of an rc allocated reference
void genlRcCounter(GenState *gen, LLVMValueRef ref, long long amount, RefNode *refnode) {
    // Point backwards to ref counter
    LLVMTypeRef ptrusize = LLVMPointerType(genlType(gen, (INode*)usizeType), 0);
    LLVMValueRef refcast = LLVMBuildBitCast(gen->builder, ref, ptrusize, "");
    LLVMValueRef minusone = LLVMConstInt(genlType(gen, (INode*)usizeType), -1, 1);
    LLVMValueRef cntptr = LLVMBuildGEP(gen->builder, refcast, &minusone, 1, "");

    // Increment ref counter
    LLVMValueRef cnt = LLVMBuildLoad(gen->builder, cntptr, "");
    LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
    LLVMValueRef newcnt = LLVMBuildAdd(gen->builder, cnt, LLVMConstInt(usize, amount, 0), "");
    LLVMBuildStore(gen->builder, newcnt, cntptr);

    // Free if zero. Otherwise, don't
    if (amount < 0) {
        LLVMBasicBlockRef nofree = genlInsertBlock(gen, "nofree");
        LLVMBasicBlockRef dofree = genlInsertBlock(gen, "free");
        LLVMValueRef test = LLVMBuildICmp(gen->builder, LLVMIntEQ, newcnt, LLVMConstInt(usize, 0, 0), "iszero");
        LLVMBuildCondBr(gen->builder, test, dofree, nofree);
        LLVMPositionBuilderAtEnd(gen->builder, dofree);
        genlDealiasFlds(gen, ref, refnode);
        genlFree(gen, cntptr);
        LLVMBuildBr(gen->builder, nofree);
        LLVMPositionBuilderAtEnd(gen->builder, nofree);
    }
}

// Progressively dealias or drop all declared variables in nodes list
void genlDealiasNodes(GenState *gen, Nodes *nodes) {
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode *)*nodesp;
        RefNode *reftype = (RefNode *)var->vtype;
        if (reftype->tag == RefTag) {
            LLVMValueRef ref = LLVMBuildLoad(gen->builder, var->llvmvar, "allocref");
            if (isRegion(reftype->region, soName)) {
                genlDealiasOwn(gen, ref, reftype);
            }
            else if (isRegion(reftype->region, rcName)) {
                genlRcCounter(gen, ref, -1, reftype);
            }
        }
    }
}
