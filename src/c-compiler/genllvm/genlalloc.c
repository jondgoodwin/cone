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

// The pointer a release routine works on. A single reference is its pointer;
// an owning slice is a fat {T*, usize} value whose pointer word is what the
// allocation header sits before.
static LLVMValueRef genlRefPtr(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    if (refnode->tag == ArrayRefTag)
        return LLVMBuildExtractValue(gen->builder, ref, 0, "sliceptr");
    return ref;
}

// If ref type is struct, dealias any fields holding rc/own references
void genlDealiasFlds(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    // A slice's elements are not walked: releasing what each element owns is
    // the same element-granularity work an array of owning references needs.
    if (refnode->tag != RefTag)
        return;
    StructNode *strnode = (StructNode*)itypeGetTypeDcl(refnode->vtexp);
    if (strnode->tag != StructTag)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        // Resolved, because a field's declared type may be a name standing for
        // the reference type rather than the reference type itself
        RefNode *vartype = (RefNode *)itypeGetTypeDcl(field->vtype);
        if (vartype->tag != RefTag || !(isRegion(vartype->region, rcName) || isRegion(vartype->region, soName)))
            continue;
        // The GEP yields the field's address; the release routines want the
        // reference the field holds, so load it.
        LLVMValueRef fldptr = LLVMBuildStructGEP(gen->builder, ref, field->index, &field->namesym->namestr);
        LLVMValueRef fldref = LLVMBuildLoad(gen->builder, fldptr, "fldref");
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

// Generate repetitive array fill of a value
void genlAllocFillArray(GenState *gen, LLVMValueRef nbrelems, ArrayNode *arraylit, LLVMValueRef valuep) {
    LLVMValueRef ptrphis[2];
    LLVMValueRef cntphis[2];
    LLVMBasicBlockRef phiblks[2];

    // Set up blocks for the upcoming loop
    LLVMBasicBlockRef loopend = genlInsertBlock(gen, "fillloopend");
    LLVMBasicBlockRef loopbody = genlInsertBlock(gen, "fillloopbody");
    LLVMBasicBlockRef loopbeg = genlInsertBlock(gen, "fillloopbeg");

    // Finish out current block
    LLVMValueRef fillval = genlExpr(gen, nodesGet(arraylit->elems, 0));
    ptrphis[0] = valuep;
    cntphis[0] = nbrelems;
    phiblks[0] = LLVMGetInsertBlock(gen->builder);
    LLVMBuildBr(gen->builder, loopbeg);

    // Code for the beginning of the loop: the exit comparison
    LLVMPositionBuilderAtEnd(gen->builder, loopbeg);
    LLVMValueRef loopptrphi = LLVMBuildPhi(gen->builder, LLVMTypeOf(valuep), "ptrphi");
    LLVMValueRef loopcntphi = LLVMBuildPhi(gen->builder, LLVMTypeOf(nbrelems), "cntphi");
    LLVMValueRef constzero = LLVMConstInt(LLVMTypeOf(loopcntphi), 0, 1);
    LLVMValueRef condbool = LLVMBuildICmp(gen->builder, LLVMIntEQ, loopcntphi, constzero, "");
    LLVMBuildCondBr(gen->builder, condbool, loopend, loopbody);

    // Store value, increment pointer and decrement counter
    LLVMPositionBuilderAtEnd(gen->builder, loopbody);
    LLVMBuildStore(gen->builder, fillval, loopptrphi);
    LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 1);
    ptrphis[1] = LLVMBuildGEP(gen->builder, loopptrphi, &constone, 1, "");
    cntphis[1] = LLVMBuildSub(gen->builder, loopcntphi, constone, "");
    phiblks[1] = loopbody;
    LLVMBuildBr(gen->builder, loopbeg);

    LLVMAddIncoming(loopptrphi, ptrphis, phiblks, 2);
    LLVMAddIncoming(loopcntphi, cntphis, phiblks, 2);
    LLVMPositionBuilderAtEnd(gen->builder, loopend);
}

// Generate region-based allocation and initialization logc
// It returns a reference to the allocated/initialized object (or null)
// This is roughly what it does:
//
// fn allocate(size usize) +region-uni T
//   imm ref = region.alloc(T.size) as +region-uni T
//   if (ref is None)
//     panic or return None
//   ref.region.init()
//   ref.perm.init()
//   T.init(&mut ref.TValue, initvalue)
//   &ref.TValue or Some[&ref.TValue]
//
LLVMValueRef genlallocref(GenState *gen, RefNode *allocatenode) {
    RefNode *reftype = (RefNode*)itypeGetTypeDcl(allocatenode->vtype);
    LLVMTypeRef reftypellvm = genlType(gen, (INode*)reftype);  // Make sure typeinfo is populated
    if (reftype->tag != RefTag && reftype->tag != ArrayRefTag) {
        // Extract reftype from Option type
        assert(reftype->tag == StructTag && (allocatenode->flags & FlagQues) && "Should be Option type");
        StructNode *optionTrait = (StructNode*)reftype;
        StructNode *someStruct = (StructNode*)nodesGet(optionTrait->derived, 1);
        // Some's first field is the tag the union gave every variant; the
        // reference is the field it declared for itself.
        INode **fldp;
        uint32_t cnt;
        for (nodelistFor(&someStruct->fields, cnt, fldp)) {
            if (!((*fldp)->flags & IsTagField))
                reftype = (RefNode*)itypeGetTypeDcl(((IExpNode*)*fldp)->vtype);
        }
        assert(reftype->tag == RefTag && "Option type did not have reftype");
    }
    INode *region = itypeGetTypeDcl(reftype->region);
    INode *perm = itypeGetTypeDcl(reftype->perm);
    LLVMTypeRef valuetypllvm = LLVMStructGetTypeAtIndex(LLVMGetElementType(reftype->typeinfo->ptrstructype), ValueField);
    LLVMTypeRef valueptrtyp = LLVMPointerType(valuetypllvm, 0);

    // Calculate how much memory space we need to allocate
    long long allocsize = LLVMABISizeOfType(gen->datalayout, reftype->typeinfo->structype);
    LLVMValueRef sizeval = LLVMConstInt(genlType(gen, (INode*)usizeType), allocsize, 0);
    LLVMValueRef nbrelems = NULL;
    if (reftype->tag == ArrayRefTag) {
        // For array-refs: sizeval += (nbrelems-1) * elemsz
        ArrayNode *initvalue = (ArrayNode*)allocatenode->vtexp;
        if (initvalue->dimens->used > 0)
            nbrelems = genlExpr(gen, nodesGet(initvalue->dimens, 0));
        else
            nbrelems = LLVMConstInt(genlType(gen, (INode*)usizeType), initvalue->elems->used, 0);
        LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
        LLVMValueRef nbrelemsdec = LLVMBuildSub(gen->builder, nbrelems, constone, "");
        LLVMValueRef elemsz = LLVMConstInt(genlType(gen, (INode*)usizeType), LLVMABISizeOfType(gen->datalayout, valuetypllvm), 0);
        LLVMValueRef extra = LLVMBuildMul(gen->builder, nbrelemsdec, elemsz, "");
        sizeval = LLVMBuildAdd(gen->builder, sizeval, extra, "");
    }

    // Do region allocation (using its alloc method) and then bitcast to multi-layered-struct ptr
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
    if (reftype->tag == ArrayRefTag) {
        LLVMValueRef tuplevalnull = LLVMGetUndef(reftypellvm);
        tuplevalnull = LLVMBuildInsertValue(gen->builder, tuplevalnull, blkvals[0], 0, "fatptr");
        blkvals[0] = LLVMBuildInsertValue(gen->builder, tuplevalnull, LLVMConstInt(genlType(gen, (INode*)usizeType), 0, 0), 1, "fatsize");
    }
    // The phi's predecessor is whatever block the builder ended up in, which is not
    // necessarily the block we positioned it in: generating the value above may have
    // emitted branches of its own, splitting the block it started in.
    blks[0] = LLVMGetInsertBlock(gen->builder);
    LLVMBuildBr(gen->builder, endif);
    LLVMPositionBuilderAtEnd(gen->builder, initblk);

    // Initialize region using its 'init' method, if supplied
    INode *reginitmeth = iTypeFindFnField(region, initMethodName);
    if (reginitmeth) {
        LLVMValueRef initval = genlFnCallInternal(gen, SimpleDispatch, (INode*)reginitmeth, 0, NULL);
        LLVMValueRef regionp = LLVMBuildStructGEP(gen->builder, ptrstructype, 0, "region");
        LLVMBuildStore(gen->builder, initval, regionp);
    }

    // Initialize permission, if it is a locked permission with an init method
    if (perm->tag == StructTag) {
        INode *perminitmeth = iTypeFindFnField(perm, initMethodName);
        if (perminitmeth) {
            LLVMValueRef initval = genlFnCallInternal(gen, SimpleDispatch, (INode*)perminitmeth, 0, NULL);
            LLVMValueRef permp = LLVMBuildStructGEP(gen->builder, ptrstructype, 1, "perm");
            LLVMBuildStore(gen->builder, initval, permp);
        }
    }

    // Initialize value (via copy or init function) and return pointer to it
    LLVMValueRef valuep = LLVMBuildStructGEP(gen->builder, ptrstructype, ValueField, ""); // Point to value
    if (reftype->tag == RefTag) {
        LLVMBuildStore(gen->builder, genlExpr(gen, allocatenode->vtexp), valuep); // Copy value
    }
    else {
        // Handle array fill via run-time generation
        if (allocatenode->vtexp->tag == ArrayLitTag && ((ArrayNode*)allocatenode->vtexp)->dimens->used > 0) {
            genlAllocFillArray(gen, nbrelems, (ArrayNode*)allocatenode->vtexp, valuep);
        }
        else {
            // Copy initial value into allocated memory area for value
            LLVMValueRef initval = genlExpr(gen, allocatenode->vtexp);
            LLVMTypeRef initvaltype = LLVMPointerType(LLVMTypeOf(initval), 0);
            LLVMValueRef valuepcast = LLVMBuildBitCast(gen->builder, valuep, initvaltype, "");
            LLVMBuildStore(gen->builder, initval, valuepcast);
        }

        // Build fat pointer for returning
        LLVMValueRef tupleval = LLVMGetUndef(reftypellvm);
        tupleval = LLVMBuildInsertValue(gen->builder, tupleval, valuep, 0, "fatptr");
        valuep = LLVMBuildInsertValue(gen->builder, tupleval, nbrelems, 1, "fatsize");
    }
    blkvals[1] = valuep;

    // Finish up block, start new one, and return allocated. As above, an initial value
    // holding another allocation splits initblk, so the edge arrives from wherever the
    // builder now is rather than from initblk.
    blks[1] = LLVMGetInsertBlock(gen->builder);
    LLVMBuildBr(gen->builder, endif);
    LLVMPositionBuilderAtEnd(gen->builder, endif);
    LLVMValueRef phi = LLVMBuildPhi(gen->builder, reftypellvm, "allocphi");
    LLVMAddIncoming(phi, blkvals, blks, 2);
    return phi;
}

// Dealias an own allocated reference
void genlDealiasOwn(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    ref = genlRefPtr(gen, ref, refnode);
    genlDealiasFlds(gen, ref, refnode);
    genlFree(gen, ref);
}

// Add to the counter of an rc allocated reference
void genlRcCounter(GenState *gen, LLVMValueRef ref, long long amount, RefNode *refnode) {
    ref = genlRefPtr(gen, ref, refnode);
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

// Release what a variable holds: free an 'so' reference, drop a holder of an
// 'rc' one, single or slice. A tuple is one holder of each owning reference it
// carries, so each is released.
void genlReleaseOwning(GenState *gen, LLVMValueRef val, INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag) {
        RefNode *reftype = (RefNode *)typedcl;
        if (isRegion(reftype->region, soName))
            genlDealiasOwn(gen, val, reftype);
        else if (isRegion(reftype->region, rcName))
            genlRcCounter(gen, val, -1, reftype);
    }
    else if (typedcl->tag == TTupleTag) {
        INode **elemp;
        uint32_t cnt;
        unsigned index = 0;
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, elemp)) {
            if (flowIsOwningType(*elemp))
                genlReleaseOwning(gen, LLVMBuildExtractValue(gen->builder, val, index, ""), *elemp);
            ++index;
        }
    }
}

// Progressively dealias or drop all declared variables in nodes list
void genlDealiasNodes(GenState *gen, Nodes *nodes) {
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp)) {
        // Hack for dealias on local variables holding region-owning reference
        if ((*nodesp)->tag == VarDclTag) {
            VarDclNode *var = (VarDclNode *)*nodesp;
            genlReleaseOwning(gen, LLVMBuildLoad(gen->builder, var->llvmvar, "allocref"), var->vtype);
        }
        // Generate function calls that drop/dealias values
        else {
            genlExpr(gen, *nodesp);
        }
    }
}
