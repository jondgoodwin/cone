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

// Function declarations for malloc() and free()
LLVMValueRef genlmallocval = NULL;
LLVMValueRef genlfreeval = NULL;

// Call malloc() (and generate declaration if needed)
LLVMValueRef genlmalloc(GenState *gen, long long size) {
    // Declare malloc() external function
    if (genlmallocval == NULL) {
        LLVMTypeRef parmtype = genlUsize(gen);
        LLVMTypeRef rettype = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
        LLVMTypeRef fnsig = LLVMFunctionType(rettype, &parmtype, 1, 0);
        genlmallocval = LLVMAddFunction(gen->module, "malloc", fnsig);
    }
    // Call malloc
    LLVMValueRef sizeval = LLVMConstInt(genlType(gen, (INode*)usizeType), size, 0);
    return LLVMBuildCall(gen->builder, genlmallocval, &sizeval, 1, "");
}

// If ref type is struct, dealias any fields holding rc/own references
void genlDealiasFlds(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    StructNode *strnode = (StructNode*)itypeGetTypeDcl(refnode->pvtype);
    if (strnode->tag != StructTag)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
        VarDclNode *field = (VarDclNode *)*nodesp;
        if (field->tag != VarDclTag)
            continue;
        RefNode *vartype = (RefNode *)field->vtype;
        if (vartype->tag != RefTag || !(vartype->alloc == (INode*)rcAlloc || vartype->alloc == (INode*)ownAlloc))
            continue;
        LLVMValueRef fldref = LLVMBuildStructGEP(gen->builder, ref, field->index, &field->namesym->namestr);
        if (vartype->alloc == (INode*)ownAlloc)
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

// Generate code that creates an allocated ref by allocating and initializing
LLVMValueRef genlallocref(GenState *gen, AllocateNode *allocatenode) {
    RefNode *reftype = (RefNode*)allocatenode->vtype;
    long long valsize = LLVMABISizeOfType(gen->datalayout, genlType(gen, reftype->pvtype));
    long long allocsize = 0;
    if (reftype->alloc == (INode*)rcAlloc)
        allocsize = LLVMABISizeOfType(gen->datalayout, genlType(gen, (INode*)usizeType));
    LLVMValueRef malloc = genlmalloc(gen, allocsize + valsize);
    if (reftype->alloc == (INode*)rcAlloc) {
        LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
        LLVMTypeRef ptrusize = LLVMPointerType(genlType(gen, (INode*)usizeType), 0);
        LLVMValueRef counterptr = LLVMBuildBitCast(gen->builder, malloc, ptrusize, "");
        LLVMBuildStore(gen->builder, constone, counterptr); // Store 1 into refcounter
        malloc = LLVMBuildGEP(gen->builder, malloc, &constone, 1, ""); // Point to value
    }
    LLVMValueRef valcast = LLVMBuildBitCast(gen->builder, malloc, genlType(gen, allocatenode->vtype), "");
    LLVMBuildStore(gen->builder, genlExpr(gen, allocatenode->exp), valcast);
    return valcast;
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
            if (reftype->alloc == (INode*)ownAlloc) {
                genlDealiasOwn(gen, ref, reftype);
            }
            else if (reftype->alloc == (INode*)rcAlloc) {
                genlRcCounter(gen, ref, -1, reftype);
            }
        }
    }
}
