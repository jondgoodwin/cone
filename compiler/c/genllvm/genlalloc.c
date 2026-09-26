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

#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

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


// The pointer a release routine works on. A single reference is its pointer;
// an owning slice is a fat {T*, usize} value whose pointer word is what the
// allocation header sits before.
static LLVMValueRef genlRefPtr(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    if (refnode->tag == ArrayRefTag)
        return LLVMBuildExtractValue(gen->builder, ref, 0, "sliceptr");
    return ref;
}

static void genlEnumDrop(GenState *gen, FnDclNode *fnnode);

// Call a type's drop on the value at 'valptr'. The pointer is recast to the
// drop's parameter type: a variant laid out as a nullable pointer is reached
// through a pointer to its enum, which is the pointer the variant is.
static void genlCallDrop(GenState *gen, INode *dropfn, LLVMValueRef valptr) {
    FnDclNode *fndcl = (FnDclNode *)dropfn;
    if (!(fndcl->flags & FlagInline) && fndcl->llvmvar != NULL) {
        LLVMTypeRef parmtype = LLVMTypeOf(LLVMGetParam(fndcl->llvmvar, 0));
        if (LLVMTypeOf(valptr) != parmtype)
            valptr = LLVMBuildBitCast(gen->builder, valptr, parmtype, "dropself");
    }
    genlFnCallInternal(gen, SimpleDispatch, dropfn, 1, &valptr, NULL);
}

// Do 'act' to each of 'count' elements of type 'elemtype', the first at
// 'firstptr', in element order: a fixed-size array's or an owning slice's. A
// loop, since the optimizer pipeline runs no loop pass that would undo an
// unrolling, and a count of zero does nothing.
typedef void (*GenlElemAct)(GenState *gen, LLVMValueRef elemptr, INode *elemtype, long long amount);
static void genlEachElem(GenState *gen, LLVMValueRef firstptr, LLVMValueRef count, INode *elemtype,
    GenlElemAct act, long long amount) {
    LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
    LLVMTypeRef elemllvm = genlType(gen, elemtype);
    LLVMBasicBlockRef entryblk = LLVMGetInsertBlock(gen->builder);
    LLVMBasicBlockRef doneblk = genlInsertBlock(gen, "elemsdone");
    LLVMBasicBlockRef loopblk = genlInsertBlock(gen, "eachelem");
    LLVMValueRef none = LLVMBuildICmp(gen->builder, LLVMIntEQ, count, LLVMConstInt(usize, 0, 0), "noelems");
    LLVMBuildCondBr(gen->builder, none, doneblk, loopblk);
    LLVMPositionBuilderAtEnd(gen->builder, loopblk);
    LLVMValueRef index = LLVMBuildPhi(gen->builder, usize, "elemindex");
    act(gen, LLVMBuildGEP2(gen->builder, elemllvm, firstptr, &index, 1, "elem"), elemtype, amount);
    LLVMValueRef next = LLVMBuildAdd(gen->builder, index, LLVMConstInt(usize, 1, 0), "elemnext");
    LLVMValueRef more = LLVMBuildICmp(gen->builder, LLVMIntULT, next, count, "elemmore");
    // What was done to the element may have split the loop's block, so the
    // back edge leaves from wherever the builder is now
    LLVMBasicBlockRef loopend = LLVMGetInsertBlock(gen->builder);
    LLVMBuildCondBr(gen->builder, more, loopblk, doneblk);
    LLVMValueRef incoming[2] = { LLVMConstInt(usize, 0, 0), next };
    LLVMBasicBlockRef fromblks[2] = { entryblk, loopend };
    LLVMAddIncoming(index, incoming, fromblks, 2);
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

// The first element of the fixed-size array at 'arrptr', and how many it has
static LLVMValueRef genlArrayFirst(GenState *gen, LLVMValueRef arrptr, INode *arraytype, LLVMValueRef *count) {
    LLVMTypeRef arrllvm = genlType(gen, arraytype);
    *count = LLVMConstInt(genlType(gen, (INode*)usizeType), LLVMGetArrayLength(arrllvm), 0);
    LLVMValueRef zeros[2];
    zeros[0] = zeros[1] = LLVMConstInt(LLVMInt32TypeInContext(gen->context), 0, 0);
    return LLVMBuildGEP2(gen->builder, arrllvm, arrptr, zeros, 2, "first");
}

static void genlFinalizeElem(GenState *gen, LLVMValueRef elemptr, INode *elemtype, long long amount) {
    genlFinalizeAt(gen, elemptr, elemtype);
}

// Finalize the value of type 'vtype' at 'valptr' where it sits, as its death
// would, without giving its memory back: the 'finalize' intrinsic, a local's
// death as its scope ends, a field's inside its holder's drop, and what an
// owning reference's death does to the value it points at before the region's
// 'free'. An owning reference is released. A struct or an enum runs its drop:
// the whole of its death, its owners' release included (genlStructDrop,
// genlEnumDrop). A tuple finalizes each element in order, and an array each
// element in element order, as a struct does its fields. A type for which
// itypeNeedsFinal is false generates nothing.
void genlFinalizeAt(GenState *gen, LLVMValueRef valptr, INode *vtype) {
    INode *typedcl = itypeGetTypeDcl(vtype);
    switch (typedcl->tag) {
    case RefTag:
    case ArrayRefTag:
        if (regionIsOwning(((RefNode *)typedcl)->region))
            genlReleaseOwning(gen, LLVMBuildLoad2(gen->builder, genlType(gen, typedcl), valptr, "finalref"), typedcl);
        return;
    case TTupleTag:
    {
        LLVMTypeRef tupllvm = genlType(gen, typedcl);
        INode **elemp;
        uint32_t cnt;
        unsigned index = 0;
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, elemp)) {
            if (itypeNeedsFinal(*elemp))
                genlFinalizeAt(gen, LLVMBuildStructGEP2(gen->builder, tupllvm, valptr, index, "tupelem"), *elemp);
            ++index;
        }
        return;
    }
    case ArrayTag:
    {
        INode *elemtype = arrayElemType(typedcl);
        if (!itypeNeedsFinal(elemtype))
            return;
        LLVMValueRef count;
        LLVMValueRef first = genlArrayFirst(gen, valptr, typedcl, &count);
        genlEachElem(gen, first, count, elemtype, genlFinalizeElem, 0);
        return;
    }
    default:
    {
        INode *dropfn = itypeGetDropFnDcl(typedcl);
        if (dropfn)
            genlCallDrop(gen, dropfn, valptr);
        return;
    }
    }
}

// The body of a struct's drop (structSetDropFn), the value 'self' points at
// dying in place in the ruled order [Jon 26 Sep]: its own 'final' and, for a
// variant keeping its enum's, the enum's -- the calls type check built as the
// body -- then each field that needs finalizing, in field order
// (genlFinalizeAt: a struct's or an enum's drop, a tuple's or an array's
// elements), then each owning reference a field holds, released in field order.
// A variant laid out as a nullable pointer is only its reference: 'self' points
// at the pointer, and releasing it is all there is.
static void genlStructDrop(GenState *gen, FnDclNode *fnnode) {
    StructNode *strnode = (StructNode*)fnnode->dclinfo.owner;
    // A variant's layout, nullable pointer or not, is its enum's decision
    INode *enumnode = strnode->basetrait ? itypeGetTypeDcl(strnode->basetrait) : NULL;
    if (enumnode && enumnode->tag == StructTag && (enumnode->flags & EnumType))
        genlType(gen, enumnode);
    LLVMTypeRef strtype = genlType(gen, (INode*)strnode);
    LLVMValueRef selfptr = LLVMGetParam(gen->fn, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((BlockNode *)fnnode->value)->stmts, cnt, nodesp))
        genlExpr(gen, *nodesp);

    if (strnode->flags & NullablePtr) {
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            INode *fldtype = itypeGetTypeDcl(((FieldDclNode *)*nodesp)->vtype);
            if (flowIsOwningType(fldtype)) {
                LLVMTypeRef refllvm = genlType(gen, fldtype);
                LLVMValueRef refptr = LLVMBuildBitCast(gen->builder, selfptr, LLVMPointerType(refllvm, 0), "nullable");
                genlReleaseOwning(gen, LLVMBuildLoad2(gen->builder, refllvm, refptr, "nullableref"), fldtype);
            }
        }
        return;
    }
    // The fields that finalize, then the owners
    for (int owners = 0; owners <= 1; ++owners) {
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            // Resolved, because a field's declared type may be a name standing
            // for the reference type rather than the reference type itself
            INode *fldtype = itypeGetTypeDcl(field->vtype);
            int isowner = (fldtype->tag == RefTag || fldtype->tag == ArrayRefTag)
                && regionIsOwning(((RefNode *)fldtype)->region);
            if (isowner != owners || !itypeNeedsFinal(fldtype))
                continue;
            genlFinalizeAt(gen, LLVMBuildStructGEP2(gen->builder, strtype, selfptr, field->index, &field->namesym->namestr), fldtype);
        }
    }
}

// The body of a drop the compiler gave a type (structIsGeneratedDropFn), built
// from the type's layout. No expression gives its calls a place, so each is
// placed at the type, where the drop was made (a call with none fails
// verification in debug).
void genlTypeDrop(GenState *gen, FnDclNode *fnnode) {
    StructNode *owner = (StructNode*)fnnode->dclinfo.owner;
    if (!gen->opt->release) {
        LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(gen->context,
            fnnode->linenbr, (unsigned)(fnnode->srcp - fnnode->linep), LLVMGetSubprogram(gen->fn), NULL);
        LLVMSetCurrentDebugLocation(gen->builder, LLVMMetadataAsValue(gen->context, loc));
    }
    if (owner->flags & EnumType)
        genlEnumDrop(gen, fnnode);
    else
        genlStructDrop(gen, fnnode);
    // A function returning nothing returns the empty value, as a 'return' does
    LLVMBuildRet(gen->builder, LLVMGetUndef(gen->emptyStructType));
}

// The fields of core's TypeRecord, in the order the compiler fills them
enum TypeRecordField {
    TypeRecSize,        // usize: the value's size
    TypeRecAlign,       // usize: the value's alignment
    TypeRecFinalize,    // &fn(p *u8): the value's death in place, less any free
    TypeRecTrace,       // &fn(p *u8, mode u32): its traced references, each handed to its region's 'mark'
    TypeRecFlags,       // u32: TypeRecFlagFinal, TypeRecFlagTraced
    TypeRecFieldCount
};
#define TypeRecFlagFinal  1u    // finalizing the value does something
#define TypeRecFlagTraced 2u    // the value holds a traced reference (itypeHoldsTraced)

// A function of type 'fntype' (a record slot's) that does nothing, one per
// object and slot, remembered at '*memo': 'cone.tyrec.nothing', what a
// record's finalizer is for a type with nothing to finalize, and
// 'cone.tyrec.untraced', what its trace is for a type holding no traced
// reference, so neither slot is ever null
static LLVMValueRef genlTypeRecNothing(GenState *gen, LLVMTypeRef fntype, LLVMValueRef *memo, char *name) {
    if (*memo)
        return *memo;
    LLVMValueRef fn = LLVMAddFunction(gen->module, name, fntype);
    LLVMSetLinkage(fn, LLVMPrivateLinkage);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(gen->context, fn, "entry"));
    LLVMBuildRet(builder, LLVMGetUndef(LLVMGetReturnType(fntype)));
    LLVMDisposeBuilder(builder);
    return *memo = fn;
}

static LLVMValueRef genlRegionHeader(GenState *gen, LLVMValueRef valptr, RefNode *refnode);

// The mode the trace being expanded was called in, handed on to each 'mark'
// that takes it; set around genlTraceAt's walk, which genlEachElem's action
// cannot be handed
static LLVMValueRef genlTraceMode = NULL;

// A traced reference is handed to its region's 'mark' as the header of what it
// points at -- with, where 'mark' asks for them, the reference's permission,
// a constant (its PermNode flags: MayRead 1, MayWrite 2, MayAlias 4,
// MayAliasWrite 8, RaceSafe 16, MayIntRefSum 32, IsLockless 64), and the mode
// the trace was called in. A null reference (a root not yet assigned, an
// absent option) is passed over.
static void genlTraceRef(GenState *gen, LLVMValueRef refptr, RefNode *refnode) {
    FnDclNode *markmeth = regionMethod(refnode->region, markMethodName);
    LLVMValueRef ref = LLVMBuildLoad2(gen->builder, genlType(gen, (INode*)refnode), refptr, "tracedref");
    // Each is placed just after the current block, so the mark lands before its join
    LLVMBasicBlockRef doneblk = genlInsertBlock(gen, "marked");
    LLVMBasicBlockRef markblk = genlInsertBlock(gen, "mark");
    LLVMBuildCondBr(gen->builder, LLVMBuildIsNotNull(gen->builder, ref, "present"), markblk, doneblk);
    LLVMPositionBuilderAtEnd(gen->builder, markblk);
    LLVMValueRef args[3];
    uint32_t nargs = 1;
    args[0] = genlRegionHeader(gen, ref, refnode);
    if (regionMarkTakesContext(refnode->region)) {
        INode *perm = itypeGetTypeDcl(refnode->perm);
        LLVMTypeRef u32 = LLVMInt32TypeInContext(gen->context);
        args[nargs++] = LLVMConstInt(u32, perm->tag == PermTag ? permGetFlags(perm) : 0, 0);
        args[nargs++] = genlTraceMode;
    }
    genlFnCallInternal(gen, SimpleDispatch, (INode*)markmeth, nargs, args, NULL);
    LLVMBuildBr(gen->builder, doneblk);
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

static void genlTraceWalk(GenState *gen, LLVMValueRef valptr, INode *vtype);

static void genlTraceElem(GenState *gen, LLVMValueRef elemptr, INode *elemtype, long long amount) {
    genlTraceWalk(gen, elemptr, elemtype);
}

// The fields of the struct 'strnode' at 'valptr' that hold a traced reference,
// each traced where it sits
static void genlTraceFields(GenState *gen, LLVMValueRef valptr, StructNode *strnode) {
    LLVMTypeRef strtype = genlType(gen, (INode*)strnode);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (itypeHoldsTraced(field->vtype))
            genlTraceWalk(gen, LLVMBuildStructGEP2(gen->builder, strtype, valptr, field->index, &field->namesym->namestr), field->vtype);
    }
}

// The value an enum (or a closed trait held by value) at 'valptr' holds is
// whichever variant its tag says, and only that variant's fields are traced:
// the same dispatch its generated drop makes (genlEnumDrop). The nullable
// pointer layout has no tag: the value is the one variant's reference, and
// null is the empty variant.
static void genlTraceVariants(GenState *gen, LLVMValueRef valptr, StructNode *enumnode) {
    INode **nodesp;
    uint32_t cnt;
    if (enumnode->flags & NullablePtr) {
        for (nodesFor(enumnode->derived, cnt, nodesp)) {
            StructNode *variant = (StructNode *)*nodesp;
            if (variant->fields.used != 2)
                continue;
            RefNode *reftype = (RefNode *)itypeGetTypeDcl(((FieldDclNode *)nodelistGet(&variant->fields, 1))->vtype);
            if (reftype->tag == RefTag && regionIsTraced(reftype->region))
                genlTraceRef(gen, valptr, reftype);
        }
        return;
    }
    FieldDclNode *tagfld = NULL;
    for (nodelistFor(&enumnode->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsTagField)
            tagfld = (FieldDclNode*)*nodesp;
    }
    if (tagfld == NULL)
        return;
    LLVMTypeRef enumtype = genlType(gen, (INode*)enumnode);
    LLVMTypeRef tagtype = genlType(gen, tagfld->vtype);
    LLVMValueRef tagptr = LLVMBuildStructGEP2(gen->builder, enumtype, valptr, tagfld->index, "tagref");
    LLVMValueRef tag = LLVMBuildLoad2(gen->builder, tagtype, tagptr, "tag");
    LLVMBasicBlockRef doneblk = genlInsertBlock(gen, "tracedone");
    LLVMValueRef dispatch = LLVMBuildSwitch(gen->builder, tag, doneblk, enumnode->derived->used);
    for (nodesFor(enumnode->derived, cnt, nodesp)) {
        StructNode *variant = (StructNode *)*nodesp;
        if (!itypeHoldsTraced((INode*)variant))
            continue;
        LLVMBasicBlockRef caseblk = genlInsertBlock(gen, "tracevariant");
        LLVMAddCase(dispatch, LLVMConstInt(tagtype, variant->tagnbr, 0), caseblk);
        LLVMPositionBuilderAtEnd(gen->builder, caseblk);
        genlTraceFields(gen, valptr, variant);
        LLVMBuildBr(gen->builder, doneblk);
    }
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

// Walk the value of type 'vtype' at 'valptr', statically and completely, and
// hand each traced reference it holds inline to its region's 'mark': a
// reference into a traced region; a tuple's elements and a struct's fields
// that hold one; each element of a fixed-size array whose element type does,
// in a loop; the variant an enum's tag picks. It stops at every other
// reference and every pointer: the placement rules keep a traced reference
// from hiding behind them (regionTracedCheckAll).
static void genlTraceWalk(GenState *gen, LLVMValueRef valptr, INode *vtype) {
    INode *typedcl = itypeGetTypeDcl(vtype);
    switch (typedcl->tag) {
    case RefTag:
        // An owning slice or virtual reference into a traced region is refused
        // (ErrorTracedRefKind), so a traced reference is a single one
        if (regionIsTraced(((RefNode *)typedcl)->region))
            genlTraceRef(gen, valptr, (RefNode *)typedcl);
        return;
    case TTupleTag:
    {
        LLVMTypeRef tupllvm = genlType(gen, typedcl);
        INode **elemp;
        uint32_t cnt;
        unsigned index = 0;
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, elemp)) {
            if (itypeHoldsTraced(*elemp))
                genlTraceWalk(gen, LLVMBuildStructGEP2(gen->builder, tupllvm, valptr, index, "tupelem"), *elemp);
            ++index;
        }
        return;
    }
    case ArrayTag:
    {
        INode *elemtype = arrayElemType(typedcl);
        if (!itypeHoldsTraced(elemtype))
            return;
        LLVMValueRef count;
        LLVMValueRef first = genlArrayFirst(gen, valptr, typedcl, &count);
        genlEachElem(gen, first, count, elemtype, genlTraceElem, 0);
        return;
    }
    case StructTag:
    {
        StructNode *strnode = (StructNode *)typedcl;
        if (strnode->derived)
            genlTraceVariants(gen, valptr, strnode);
        else
            genlTraceFields(gen, valptr, strnode);
        return;
    }
    default:
        return;
    }
}

// Hand each traced reference the value of type 'vtype' at 'valptr' holds to
// its region's 'mark', with 'mode' where 'mark' takes it: what 'mem.trace'
// expands to, and the body of a record's trace. Nothing, for a type holding
// no traced reference.
void genlTraceAt(GenState *gen, LLVMValueRef valptr, INode *vtype, LLVMValueRef mode) {
    if (!itypeHoldsTraced(vtype))
        return;
    LLVMValueRef svmode = genlTraceMode;
    genlTraceMode = mode;
    genlTraceWalk(gen, valptr, vtype);
    genlTraceMode = svmode;
}

// A record's function for 'vtype' (its finalizer or its trace), of type
// 'fntype' and named 'name': 'body' generates what it does to the value at its
// first parameter. It is generated as a function of its own, so the
// generator's state for the function being generated is set aside around it,
// as genlFn does for a nested one.
typedef void (*GenlTypeRecBody)(GenState *gen, LLVMValueRef fn, INode *vtype);
static LLVMValueRef genlTypeRecFn(GenState *gen, INode *vtype, LLVMTypeRef fntype, char *name, GenlTypeRecBody body) {
    LLVMValueRef fn = LLVMAddFunction(gen->module, name, fntype);
    LLVMSetLinkage(fn, LLVMPrivateLinkage);

    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    INode *svfnblock = gen->fnblock;
    int svexitzero = gen->exitzero;
    GenRoots svroots;
    genlRootsSave(gen, &svroots);
    gen->fn = fn;
    gen->fnblock = NULL;
    gen->exitzero = 0;

    // A call it makes to an inlinable function needs a location in a debug
    // build, so the function has a subprogram, placed at the type
    INode *typedcl = itypeGetTypeDcl(vtype);
    if (!gen->opt->release) {
        LLVMMetadataRef sptype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder, gen->difile, NULL, 0, 0);
        LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
            name, strlen(name), name, strlen(name), gen->difile, typedcl->linenbr, sptype, 1, 1, typedcl->linenbr, 0, 0);
        LLVMSetSubprogram(fn, sp);
    }
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(gen->builder, LLVMAppendBasicBlockInContext(gen->context, fn, "entry"));
    if (!gen->opt->release) {
        unsigned col = typedcl->srcp && typedcl->linep ? (unsigned)(typedcl->srcp - typedcl->linep) : 0;
        LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(gen->context, typedcl->linenbr, col, LLVMGetSubprogram(fn), NULL);
        LLVMSetCurrentDebugLocation2(gen->builder, loc);
    }
    gen->allocaPoint = LLVMBuildAlloca(gen->builder, LLVMInt32TypeInContext(gen->context), "alloca_point");

    body(gen, fn, vtype);
    LLVMBuildRet(gen->builder, LLVMGetUndef(LLVMGetReturnType(fntype)));
    // Rooted like any function, should what it expands hold a traced reference
    genlRootFrame(gen);

    if (LLVMGetInstructionParent(gen->allocaPoint))
        LLVMInstructionEraseFromParent(gen->allocaPoint);
    LLVMDisposeBuilder(gen->builder);
    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fnblock = svfnblock;
    gen->exitzero = svexitzero;
    genlRootsRestore(gen, &svroots);
    return fn;
}

// The finalizer's body: the value's death in place at the address it is
// handed, as 'mem.finalize' expands it (genlFinalizeAt): its 'final', its
// fields that need it, then the owners it holds
static void genlTypeRecFinalBody(GenState *gen, LLVMValueRef fn, INode *vtype) {
    genlFinalizeAt(gen, LLVMGetParam(fn, 0), vtype);
}

// The trace's body: each traced reference in the value at the address it is
// handed, given to its region's 'mark' with the mode it was called in, as
// 'mem.trace' expands it (genlTraceAt)
static void genlTypeRecTraceBody(GenState *gen, LLVMValueRef fn, INode *vtype) {
    genlTraceAt(gen, LLVMGetParam(fn, 0), vtype, LLVMGetParam(fn, 1));
}

// Whether core's TypeRecord is laid out as the compiler fills it: two usizes,
// two pointers to functions, and a u32; the finalizer's function taking a
// pointer, the trace's a pointer and a u32
static int genlTypeRecLayoutOk(GenState *gen, LLVMTypeRef rectype, LLVMTypeRef finaltype, LLVMTypeRef tracetype) {
    if (LLVMGetTypeKind(rectype) != LLVMStructTypeKind || LLVMCountStructElementTypes(rectype) != TypeRecFieldCount)
        return 0;
    LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
    LLVMTypeRef u32 = LLVMInt32TypeInContext(gen->context);
    LLVMTypeRef traceparms[2];
    if (tracetype == NULL || LLVMCountParamTypes(tracetype) != 2)
        return 0;
    LLVMGetParamTypes(tracetype, traceparms);
    return LLVMStructGetTypeAtIndex(rectype, TypeRecSize) == usize
        && LLVMStructGetTypeAtIndex(rectype, TypeRecAlign) == usize
        && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(rectype, TypeRecFinalize)) == LLVMPointerTypeKind
        && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(rectype, TypeRecTrace)) == LLVMPointerTypeKind
        && LLVMStructGetTypeAtIndex(rectype, TypeRecFlags) == u32
        && finaltype != NULL && LLVMCountParamTypes(finaltype) == 1
        && LLVMGetTypeKind(traceparms[0]) == LLVMPointerTypeKind && traceparms[1] == u32;
}

// The function type a record's finalize or trace slot points at: 'fn(p *u8)',
// or 'fn(p *u8, mode u32)', read from core's declaration of the slot, so a
// call through it is typed as the slot is
static LLVMTypeRef genlTypeRecSlotFnType(GenState *gen, StructNode *recnode, unsigned index) {
    if (recnode->fields.used != TypeRecFieldCount)
        return NULL;
    INode *slottype = itypeGetTypeDcl(((FieldDclNode *)nodelistGet(&recnode->fields, index))->vtype);
    if (slottype->tag != RefTag)
        return NULL;
    INode *fnsig = itypeGetTypeDcl(((RefNode *)slottype)->vtexp);
    return fnsig->tag == FnSigTag ? genlType(gen, fnsig) : NULL;
}

// The type record of 'vtype': a private constant of core's TypeRecord, what a
// region's 'alloc(size usize, ty *TypeRecord)' is handed and what
// 'mem.typeRecord[T]()' is the address of. Its size and alignment are the
// target's (as mem.sizeof and mem.alignof); its finalizer is the value's death
// in place (genlTypeRecFinalBody), or the shared do-nothing function where
// finalizing does nothing, which its flags say too; its trace hands each
// traced reference the value holds to its region's 'mark' (genlTypeRecTraceBody),
// or is the shared do-nothing trace where it holds none, which its flags say
// too. Built once per type in
// each object, so two objects hold two records of one type: nothing compares
// records by address. 'recptrtype' is the '*TypeRecord' the caller declared,
// which names the struct to build.
LLVMValueRef genlTypeRecord(GenState *gen, INode *vtype, INode *recptrtype) {
    return genlTypeRecordOf(gen, vtype, (StructNode *)itypeGetTypeDcl(((StarNode *)itypeGetTypeDcl(recptrtype))->vtexp));
}

// The same, handed core's TypeRecord struct itself: a root map's records,
// which no declaration names (genlRootFrame)
LLVMValueRef genlTypeRecordOf(GenState *gen, INode *vtype, StructNode *recnode) {
    for (uint32_t i = 0; i < gen->tyreccnt; ++i) {
        if (itypeIsSame(gen->tyrectypes[i], vtype))
            return gen->tyrecs[i];
    }

    LLVMTypeRef rectype = genlType(gen, (INode*)recnode);
    LLVMTypeRef finaltype = genlTypeRecSlotFnType(gen, recnode, TypeRecFinalize);
    LLVMTypeRef tracetype = genlTypeRecSlotFnType(gen, recnode, TypeRecTrace);
    if (!genlTypeRecLayoutOk(gen, rectype, finaltype, tracetype))
        errorExit(ExitGen, "Internal error: core's TypeRecord is not laid out as the compiler fills it: "
            "'size usize; align usize; finalize &fn(p *u8); trace &fn(p *u8, mode u32); flags u32;'");

    // The record is remembered before its finalizer and trace are generated,
    // which may ask for records of their own, this one among them
    uint32_t index = gen->tyreccnt;
    char name[64];
    sprintf(name, "cone.tyrec.%u", index);
    LLVMValueRef record = LLVMAddGlobal(gen->module, rectype, name);
    LLVMSetGlobalConstant(record, 1);
    LLVMSetLinkage(record, LLVMPrivateLinkage);
    if (index == gen->tyrecmax) {
        uint32_t newmax = gen->tyrecmax ? gen->tyrecmax * 2 : 16;
        INode **types = (INode **)memAllocBlk(newmax * sizeof(INode *));
        LLVMValueRef *recs = (LLVMValueRef *)memAllocBlk(newmax * sizeof(LLVMValueRef));
        if (index) {
            memcpy(types, gen->tyrectypes, index * sizeof(INode *));
            memcpy(recs, gen->tyrecs, index * sizeof(LLVMValueRef));
        }
        gen->tyrectypes = types;
        gen->tyrecs = recs;
        gen->tyrecmax = newmax;
    }
    gen->tyrectypes[index] = vtype;
    gen->tyrecs[index] = record;
    gen->tyreccnt = index + 1;

    int needsfinal = itypeNeedsFinal(vtype);
    int traced = itypeHoldsTraced(vtype);
    LLVMTypeRef valtype = genlType(gen, vtype);
    LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
    LLVMValueRef fields[TypeRecFieldCount];
    fields[TypeRecSize] = LLVMConstInt(usize, LLVMABISizeOfType(gen->datalayout, valtype), 0);
    fields[TypeRecAlign] = LLVMConstInt(usize, LLVMABIAlignmentOfType(gen->datalayout, valtype), 0);
    if (needsfinal) {
        sprintf(name, "cone.tyrec.final.%u", index);
        fields[TypeRecFinalize] = genlTypeRecFn(gen, vtype, finaltype, name, genlTypeRecFinalBody);
    }
    else
        fields[TypeRecFinalize] = genlTypeRecNothing(gen, finaltype, &gen->tyrecnothing, "cone.tyrec.nothing");
    if (traced) {
        sprintf(name, "cone.tyrec.trace.%u", index);
        fields[TypeRecTrace] = genlTypeRecFn(gen, vtype, tracetype, name, genlTypeRecTraceBody);
    }
    else
        fields[TypeRecTrace] = genlTypeRecNothing(gen, tracetype, &gen->tyrecuntraced, "cone.tyrec.untraced");
    fields[TypeRecFlags] = LLVMConstInt(LLVMInt32TypeInContext(gen->context),
        (needsfinal ? TypeRecFlagFinal : 0) | (traced ? TypeRecFlagTraced : 0), 0);
    LLVMSetInitializer(record, LLVMConstNamedStruct(rectype, fields, TypeRecFieldCount));
    return record;
}

static void genlAliasElem(GenState *gen, LLVMValueRef elemptr, INode *elemtype, long long amount) {
    genlAliasHeld(gen, elemptr, elemtype, amount);
}

// A value at 'valptr' was copied: each counted reference its death releases
// gains 'amount' holders (flowHeldCounted). The mirror of that death: a counted
// reference itself, each element of a tuple or an array, each field of a
// struct, and in an enum, each field of the variant the tag picks.
void genlAliasHeld(GenState *gen, LLVMValueRef valptr, INode *type, long long amount) {
    INode *typedcl = itypeGetTypeDcl(type);
    INode **nodesp;
    uint32_t cnt;
    switch (typedcl->tag) {
    case RefTag:
    case ArrayRefTag:
        if (flowIsRcRef(typedcl))
            genlRegionAlias(gen, LLVMBuildLoad2(gen->builder, genlType(gen, typedcl), valptr, "heldref"), amount, (RefNode *)typedcl);
        return;
    case TTupleTag:
    {
        LLVMTypeRef tupllvm = genlType(gen, typedcl);
        unsigned index = 0;
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, nodesp)) {
            if (flowIsRcRef(*nodesp) || flowHeldCounted(*nodesp))
                genlAliasHeld(gen, LLVMBuildStructGEP2(gen->builder, tupllvm, valptr, index, ""), *nodesp, amount);
            ++index;
        }
        return;
    }
    case ArrayTag:
    {
        INode *elemtype = arrayElemType(typedcl);
        if (!flowIsRcRef(elemtype) && !flowHeldCounted(elemtype))
            return;
        LLVMValueRef count;
        LLVMValueRef first = genlArrayFirst(gen, valptr, typedcl, &count);
        genlEachElem(gen, first, count, elemtype, genlAliasElem, amount);
        return;
    }
    case StructTag:
        break;
    default:
        return;
    }
    StructNode *strnode = (StructNode *)typedcl;
    LLVMTypeRef strtype = genlType(gen, (INode*)strnode);
    if (!(strnode->flags & EnumType)) {
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            if (flowIsRcRef(field->vtype) || flowHeldCounted(field->vtype))
                genlAliasHeld(gen, LLVMBuildStructGEP2(gen->builder, strtype, valptr, field->index, ""), field->vtype, amount);
        }
        return;
    }

    LLVMBasicBlockRef doneblk = genlInsertBlock(gen, "aliasheld");
    if (strnode->flags & NullablePtr) {
        // The one variant with a field is the reference; null is the empty one
        for (nodesFor(strnode->derived, cnt, nodesp)) {
            StructNode *variant = (StructNode *)*nodesp;
            if (variant->fields.used != 2 || !flowVariantHeldCounted((INode*)variant))
                continue;
            RefNode *reftype = (RefNode *)itypeGetTypeDcl(((FieldDclNode *)nodelistGet(&variant->fields, 1))->vtype);
            LLVMValueRef ref = LLVMBuildLoad2(gen->builder, strtype, valptr, "nullable");
            LLVMBasicBlockRef someblk = genlInsertBlock(gen, "aliassome");
            LLVMBuildCondBr(gen->builder, LLVMBuildIsNotNull(gen->builder, ref, "present"), someblk, doneblk);
            LLVMPositionBuilderAtEnd(gen->builder, someblk);
            genlRegionAlias(gen, ref, amount, reftype);
        }
        LLVMBuildBr(gen->builder, doneblk);
        LLVMPositionBuilderAtEnd(gen->builder, doneblk);
        return;
    }

    FieldDclNode *tagfld = NULL;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsTagField)
            tagfld = (FieldDclNode*)*nodesp;
    }
    LLVMTypeRef tagtype = genlType(gen, tagfld->vtype);
    LLVMValueRef tagptr = LLVMBuildStructGEP2(gen->builder, strtype, valptr, tagfld->index, "tagref");
    LLVMValueRef dispatch = LLVMBuildSwitch(gen->builder, LLVMBuildLoad2(gen->builder, tagtype, tagptr, "tag"),
        doneblk, strnode->derived->used);
    for (nodesFor(strnode->derived, cnt, nodesp)) {
        StructNode *variant = (StructNode *)*nodesp;
        if (!flowVariantHeldCounted((INode*)variant))
            continue;
        LLVMBasicBlockRef caseblk = genlInsertBlock(gen, "aliasvariant");
        LLVMAddCase(dispatch, LLVMConstInt(tagtype, variant->tagnbr, 0), caseblk);
        LLVMPositionBuilderAtEnd(gen->builder, caseblk);
        LLVMTypeRef vartype = genlType(gen, (INode*)variant);
        LLVMValueRef varptr = LLVMBuildBitCast(gen->builder, valptr, LLVMPointerType(vartype, 0), "variant");
        INode **fldp;
        uint32_t fldcnt;
        for (nodelistFor(&variant->fields, fldcnt, fldp)) {
            FieldDclNode *field = (FieldDclNode *)*fldp;
            LLVMValueRef fldptr;
            if (flowIsRcRef(field->vtype) || flowHeldCounted(field->vtype)) {
                fldptr = LLVMBuildStructGEP2(gen->builder, vartype, varptr, field->index, "");
                genlAliasHeld(gen, fldptr, field->vtype, amount);
            }
        }
        LLVMBuildBr(gen->builder, doneblk);
    }
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

// The body of an enum's drop (structSetEnumDropFn): the value 'self' points at
// dies as the variant it holds would, finalized in place (genlFinalizeAt): the
// variant's drop -- its own 'final', the enum's 'final', each finalizing field's
// death, then the owning references its fields hold, the common fields' among
// them. The tag picks the variant, and a variant with nothing to do has no case.
// The nullable-pointer layout has no tag: the value is the one variant's
// reference, and null is the empty variant, which has nothing to do.
static void genlEnumDrop(GenState *gen, FnDclNode *fnnode) {
    StructNode *enumnode = (StructNode*)fnnode->dclinfo.owner;
    LLVMValueRef selfptr = LLVMGetParam(gen->fn, 0);
    LLVMTypeRef enumtype = genlType(gen, (INode*)enumnode);
    LLVMBasicBlockRef doneblk = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "dropdone");
    INode **nodesp;
    uint32_t cnt;

    if (enumnode->flags & NullablePtr) {
        StructNode *somenode = NULL;
        for (nodesFor(enumnode->derived, cnt, nodesp)) {
            if (((StructNode*)*nodesp)->fields.used == 2)
                somenode = (StructNode*)*nodesp;
        }
        LLVMValueRef ptr = LLVMBuildLoad2(gen->builder, enumtype, selfptr, "nullable");
        if (LLVMGetTypeKind(enumtype) != LLVMPointerTypeKind)
            ptr = LLVMBuildExtractValue(gen->builder, ptr, 0, "ptr");   // a fat pointer's
        LLVMValueRef present = LLVMBuildIsNotNull(gen->builder, ptr, "present");
        LLVMBasicBlockRef someblk = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "dropsome");
        LLVMBuildCondBr(gen->builder, present, someblk, doneblk);
        LLVMPositionBuilderAtEnd(gen->builder, someblk);
        genlFinalizeAt(gen, selfptr, (INode*)somenode);
        LLVMBuildBr(gen->builder, doneblk);
    }
    else {
        FieldDclNode *tagfld = NULL;
        for (nodelistFor(&enumnode->fields, cnt, nodesp)) {
            if ((*nodesp)->flags & IsTagField)
                tagfld = (FieldDclNode*)*nodesp;
        }
        LLVMTypeRef tagtype = genlType(gen, tagfld->vtype);
        LLVMValueRef tagptr = LLVMBuildStructGEP2(gen->builder, enumtype, selfptr, tagfld->index, "tagref");
        LLVMValueRef tag = LLVMBuildLoad2(gen->builder, tagtype, tagptr, "tag");
        LLVMValueRef dispatch = LLVMBuildSwitch(gen->builder, tag, doneblk, enumnode->derived->used);
        for (nodesFor(enumnode->derived, cnt, nodesp)) {
            StructNode *variant = (StructNode*)*nodesp;
            if (!itypeNeedsFinal((INode*)variant))
                continue;
            LLVMBasicBlockRef caseblk = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "dropvariant");
            LLVMAddCase(dispatch, LLVMConstInt(tagtype, variant->tagnbr, 0), caseblk);
            LLVMPositionBuilderAtEnd(gen->builder, caseblk);
            LLVMValueRef varptr = LLVMBuildBitCast(gen->builder, selfptr,
                LLVMPointerType(genlType(gen, (INode*)variant), 0), "variant");
            genlFinalizeAt(gen, varptr, (INode*)variant);
            LLVMBuildBr(gen->builder, doneblk);
        }
    }
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

// The region's header for the value an owning reference points at: the
// allocation's first field, which is where 'alloc' returned. It sits before
// the value by the offset of the value in the {region, permission, value}
// layout, so the address is computed from that layout rather than assuming any
// region's or permission's size. This is the 'self' every region method but
// 'alloc' and 'init' is handed.
static LLVMValueRef genlRegionHeader(GenState *gen, LLVMValueRef valptr, RefNode *refnode) {
    genlType(gen, (INode*)refnode);    // Make sure typeinfo is populated
    unsigned long long offset = LLVMOffsetOfElement(gen->datalayout, refnode->typeinfo->structype, ValueField);
    LLVMTypeRef hdrptrtype = LLVMPointerType(genlType(gen, refnode->region), 0);
    if (offset == 0)
        return LLVMBuildBitCast(gen->builder, valptr, hdrptrtype, "header");
    LLVMTypeRef bytetype = LLVMInt8TypeInContext(gen->context);
    LLVMValueRef bytep = LLVMBuildBitCast(gen->builder, valptr, LLVMPointerType(bytetype, 0), "");
    LLVMValueRef back = LLVMConstInt(genlType(gen, (INode*)usizeType), -(long long)offset, 1);
    bytep = LLVMBuildGEP2(gen->builder, bytetype, bytep, &back, 1, "");
    return LLVMBuildBitCast(gen->builder, bytep, hdrptrtype, "header");
}

// Call a region method that takes the header as 'self'
static LLVMValueRef genlRegionCall(GenState *gen, FnDclNode *meth, LLVMValueRef header) {
    return genlFnCallInternal(gen, SimpleDispatch, (INode*)meth, 1, &header, NULL);
}

// A path from a value inwards to a part of it that was moved out: each step is
// the node that took it -- an element index or a dereference, never a field
// access -- outermost last, so steps[0] is the first step inside the value.
typedef struct {
    INode **steps;
    int len;
} MovedPath;

static void genlHollowDeath(GenState *gen, LLVMValueRef valptr, RefNode *refnode, MovedPath *paths, int npaths, int depth);

// The value an owning reference points at is dead: finalize it in place, as a
// value on the stack is (genlFinalizeAt: its 'final', its fields that need
// it, the owners it holds), then give the memory back through the region's
// 'free', where it has one. An owning slice's elements each die so, in element
// order, its length read from 'ref'. With 'paths', the value or a part of it
// was moved out, and it dies hollow (genlHollowDeath) instead.
static void genlRegionDeath(GenState *gen, LLVMValueRef ref, LLVMValueRef valptr, RefNode *refnode, MovedPath *paths, int npaths, int depth) {
    if (paths) {
        genlHollowDeath(gen, valptr, refnode, paths, npaths, depth);
        return;
    }
    if (refnode->tag == RefTag)
        genlFinalizeAt(gen, valptr, refnode->vtexp);
    else if (itypeNeedsFinal(refnode->vtexp))
        genlEachElem(gen, valptr, LLVMBuildExtractValue(gen->builder, ref, 1, "slicelen"), refnode->vtexp, genlFinalizeElem, 0);
    FnDclNode *freemeth = regionMethod(refnode->region, freeMethodName);
    if (freemeth)
        genlRegionCall(gen, freemeth, genlRegionHeader(gen, valptr, refnode));
}

// One owner of an owning reference goes away. A region with 'dealias' is asked
// whether it was the last, and the value dies only if so. Without one, a 'Move'
// region's owner is the only one, and its going is the value's death; any
// other region's owner going is nothing at all [Jon 26 Sep]: such a value never
// dies by an owner, and is left to the region, in its own loop, or to nothing,
// to free. 'paths' (NULL for a whole value) are the parts moved out of what it
// points at, starting at 'depth'.
static void genlRegionDealiasPart(GenState *gen, LLVMValueRef ref, RefNode *refnode, MovedPath *paths, int npaths, int depth) {
    if (!regionReleaseActs(refnode->region))
        return;
    FnDclNode *dealiasmeth = regionMethod(refnode->region, dealiasMethodName);
    LLVMValueRef valptr = genlRefPtr(gen, ref, refnode);
    if (dealiasmeth == NULL) {
        genlRegionDeath(gen, ref, valptr, refnode, paths, npaths, depth);
        return;
    }
    LLVMValueRef last = genlRegionCall(gen, dealiasmeth, genlRegionHeader(gen, valptr, refnode));
    LLVMBasicBlockRef nofree = genlInsertBlock(gen, "nofree");
    LLVMBasicBlockRef dofree = genlInsertBlock(gen, "free");
    LLVMBuildCondBr(gen->builder, last, dofree, nofree);
    LLVMPositionBuilderAtEnd(gen->builder, dofree);
    genlRegionDeath(gen, ref, valptr, refnode, paths, npaths, depth);
    LLVMBuildBr(gen->builder, nofree);
    LLVMPositionBuilderAtEnd(gen->builder, nofree);
}

static void genlRegionDealias(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    genlRegionDealiasPart(gen, ref, refnode, NULL, 0, 0);
}

// Release a value in memory at 'ptr' of type 'type', out of which something
// was moved ('paths', from 'depth' on). A path that ends here says the whole
// value moved: nothing is left to release. Nothing moves out of a field
// (flowRefuseMoveField), so a path runs on past a value only through an owning
// reference -- '**b' took what a referent's own owning reference points at --
// and that reference dies hollow in turn. Anything else a path runs through --
// an array element -- is not finalized at all: which elements are left is not
// tracked, so the ones that did not move leak rather than one being
// finalized twice.
static void genlReleasePart(GenState *gen, LLVMValueRef ptr, INode *type, MovedPath *paths, int npaths, int depth) {
    for (int i = 0; i < npaths; ++i) {
        if (paths[i].len <= depth)
            return;
    }
    INode *typedcl = itypeGetTypeDcl(type);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag) {
        RefNode *reftype = (RefNode *)typedcl;
        if (!regionIsOwning(reftype->region))
            return;
        genlRegionDealiasPart(gen, LLVMBuildLoad2(gen->builder, genlType(gen, typedcl), ptr, "partref"), reftype, paths, npaths, depth);
    }
}

// The value an owning reference points at dies with it, or a part of it, moved
// out: no finalizer runs for it, what is left is released (genlReleasePart),
// and the memory goes back through the region's 'free'. A path of one step
// moved the whole value out, leaving only the memory. A slice's elements are
// not walked: one of them moved, and which is not tracked.
static void genlHollowDeath(GenState *gen, LLVMValueRef valptr, RefNode *refnode, MovedPath *paths, int npaths, int depth) {
    if (refnode->tag == RefTag)
        genlReleasePart(gen, valptr, refnode->vtexp, paths, npaths, depth + 1);
    FnDclNode *freemeth = regionMethod(refnode->region, freeMethodName);
    if (freemeth)
        genlRegionCall(gen, freemeth, genlRegionHeader(gen, valptr, refnode));
}

// The steps from a variable out to what a hollowing move took, found by
// walking the move's chain inwards to the variable (flowHollowOwner's walk)
static MovedPath genlMovedPath(INode *moved, VarDclNode *var) {
    MovedPath path;
    int len = 0;
    for (INode *exp = moved; !(isNameUseNode(exp) && isExpNode(exp)); ) {
        switch (exp->tag) {
        case ArrIndexTag:
            ++len; exp = ((FnCallNode *)exp)->objfn; break;
        case DerefTag:
            ++len; exp = ((StarNode *)exp)->vtexp; break;
        case CastTag:
            exp = ((CastNode *)exp)->exp; break;
        default:
            errorUnreachable(moved, "a hollowing move whose chain does not reach its variable");
            path.steps = NULL; path.len = 0;
            return path;
        }
    }
    path.steps = (INode **)memAllocBlk(len * sizeof(INode *));
    path.len = len;
    int pos = len;
    for (INode *exp = moved; !(isNameUseNode(exp) && isExpNode(exp)); ) {
        switch (exp->tag) {
        case ArrIndexTag:
            path.steps[--pos] = exp; exp = ((FnCallNode *)exp)->objfn; break;
        case DerefTag:
            path.steps[--pos] = exp; exp = ((StarNode *)exp)->vtexp; break;
        default:
            exp = ((CastNode *)exp)->exp; break;
        }
    }
    return path;
}

// A hollowed variable's release: its owning reference goes away as any owner
// does, and if that is the value's death, it dies hollow
void genlHollowRelease(GenState *gen, HollowNode *hnode) {
    VarDclNode *var = hnode->var;
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(var->vtype);
    int npaths = hnode->moved->used;
    MovedPath *paths = (MovedPath *)memAllocBlk(npaths * sizeof(MovedPath));
    for (int i = 0; i < npaths; ++i)
        paths[i] = genlMovedPath(nodesGet(hnode->moved, i), var);
    LLVMValueRef ref = LLVMBuildLoad2(gen->builder, genlType(gen, var->vtype), var->llvmvar, "hollowref");
    genlRegionDealiasPart(gen, ref, reftype, paths, npaths, 0);
}

// Up to this many owners gained at once, 'alias' is called in line, once for
// each: the optimizer pipeline (genllvm.c) runs no loop pass, so only calls
// written out fold, for an 'alias' that adds to a count, into one addition
#define RegionAliasUnroll 16

// A counted reference gains 'amount' owners: its region's 'alias' is called
// once for each. Only an array fill literal makes more than one at once, up to
// INT16_MAX (arraylit.c); beyond RegionAliasUnroll that is a loop. A negative
// amount is owners going away: a fill literal of no elements drops the
// temporary it was given.
void genlRegionAlias(GenState *gen, LLVMValueRef ref, long long amount, RefNode *refnode) {
    if (amount < 0) {
        for (long long i = 0; i < -amount; ++i)
            genlRegionDealias(gen, ref, refnode);
        return;
    }
    FnDclNode *aliasmeth = regionMethod(refnode->region, aliasMethodName);
    if (aliasmeth == NULL || amount == 0)
        return;
    LLVMValueRef header = genlRegionHeader(gen, genlRefPtr(gen, ref, refnode), refnode);
    if (amount <= RegionAliasUnroll) {
        for (long long i = 0; i < amount; ++i)
            genlRegionCall(gen, aliasmeth, header);
        return;
    }
    LLVMTypeRef usize = genlType(gen, (INode*)usizeType);
    LLVMBasicBlockRef entryblk = LLVMGetInsertBlock(gen->builder);
    LLVMBasicBlockRef doneblk = genlInsertBlock(gen, "aliasdone");
    LLVMBasicBlockRef loopblk = genlInsertBlock(gen, "aliasloop");
    LLVMBuildBr(gen->builder, loopblk);
    LLVMPositionBuilderAtEnd(gen->builder, loopblk);
    LLVMValueRef counter = LLVMBuildPhi(gen->builder, usize, "aliascount");
    genlRegionCall(gen, aliasmeth, header);
    LLVMValueRef next = LLVMBuildAdd(gen->builder, counter, LLVMConstInt(usize, 1, 0), "aliasnext");
    LLVMValueRef more = LLVMBuildICmp(gen->builder, LLVMIntULT, next, LLVMConstInt(usize, amount, 0), "aliasmore");
    // The call may have split the loop's block, so the back edge leaves from
    // wherever the builder is now
    LLVMBasicBlockRef loopend = LLVMGetInsertBlock(gen->builder);
    LLVMBuildCondBr(gen->builder, more, loopblk, doneblk);
    LLVMValueRef incoming[2] = { LLVMConstInt(usize, 0, 0), next };
    LLVMBasicBlockRef fromblks[2] = { entryblk, loopend };
    LLVMAddIncoming(counter, incoming, fromblks, 2);
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
}

// Generate repetitive array fill of a value, each element of LLVM type 'elemtype'
void genlAllocFillArray(GenState *gen, LLVMValueRef nbrelems, ArrayNode *arraylit, LLVMValueRef valuep, LLVMTypeRef elemtype) {
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
    ptrphis[1] = LLVMBuildGEP2(gen->builder, elemtype, loopptrphi, &constone, 1, "");
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
        // A fallible allocation is typed 'Option[&T]', and what is wanted here is
        // the '&T' that wrapping hid. It is the field of whichever variant carries
        // one: every variant's first field is the discriminant the enum gave it,
        // and only the value-carrying variant declares another. Found that way
        // rather than by position or by name, so neither the order Option declares
        // its variants in nor what they are called is baked in here.
        assert(reftype->tag == StructTag && (allocatenode->flags & FlagQues) && "Should be Option type");
        StructNode *optionEnum = (StructNode*)reftype;
        INode **variantp;
        uint32_t variantcnt;
        for (nodesFor(optionEnum->derived, variantcnt, variantp)) {
            INode **fldp;
            uint32_t cnt;
            for (nodelistFor(&((StructNode*)*variantp)->fields, cnt, fldp)) {
                if (!((*fldp)->flags & IsTagField))
                    reftype = (RefNode*)itypeGetTypeDcl(((IExpNode*)*fldp)->vtype);
            }
        }
        assert(reftype->tag == RefTag && "Option type did not have reftype");
    }
    INode *region = itypeGetTypeDcl(reftype->region);
    INode *perm = itypeGetTypeDcl(reftype->perm);
    LLVMTypeRef valuetypllvm = LLVMStructGetTypeAtIndex(reftype->typeinfo->structype, ValueField);
    LLVMTypeRef valueptrtyp = LLVMPointerType(valuetypllvm, 0);

    // Calculate how much memory space we need to allocate
    long long allocsize = LLVMABISizeOfType(gen->datalayout, reftype->typeinfo->structype);
    LLVMValueRef sizeval = LLVMConstInt(genlType(gen, (INode*)usizeType), allocsize, 0);
    LLVMValueRef nbrelems = NULL;
    if (reftype->tag == ArrayRefTag) {
        // For array-refs: sizeval += (nbrelems-1) * elemsz
        // Only a fill literal's count is known at run time. Any other initial
        // value -- a listed array literal, a string literal, a variable holding
        // an array -- is typed a fixed-size array, whose dimension is the count.
        INode *initvalue = allocatenode->vtexp;
        if (initvalue->tag == ArrayLitTag && ((ArrayNode*)initvalue)->dimens->used > 0)
            nbrelems = genlExpr(gen, nodesGet(((ArrayNode*)initvalue)->dimens, 0));
        else
            nbrelems = LLVMConstInt(genlType(gen, (INode*)usizeType), arrayDim1(iexpGetTypeDcl(initvalue)), 0);
        LLVMValueRef constone = LLVMConstInt(genlType(gen, (INode*)usizeType), 1, 0);
        LLVMValueRef nbrelemsdec = LLVMBuildSub(gen->builder, nbrelems, constone, "");
        LLVMValueRef elemsz = LLVMConstInt(genlType(gen, (INode*)usizeType), LLVMABISizeOfType(gen->datalayout, valuetypllvm), 0);
        LLVMValueRef extra = LLVMBuildMul(gen->builder, nbrelemsdec, elemsz, "");
        sizeval = LLVMBuildAdd(gen->builder, sizeval, extra, "");
    }

    // A traced region's value is evaluated before its 'alloc' is called: an
    // 'alloc' may collect, and a value that allocates ('+gc Pair[+gc Leaf[1],
    // ...]') would otherwise run that collection with the new object linked in
    // and holding garbage. Its traced parts are births, so rooted while 'alloc'
    // runs. Every other region keeps the order it always had: 'alloc', then
    // the value, evaluated straight into the new memory.
    LLVMValueRef tracedval = NULL;
    if (reftype->tag == RefTag && regionIsTraced((INode*)region))
        tracedval = genlExpr(gen, allocatenode->vtexp);

    // Do region allocation (using its alloc method) and then bitcast to multi-layered-struct ptr.
    // An 'alloc' that asks for it is handed the value type's record after the
    // size: an owning slice's is its element type's. One that does not is
    // called with the size alone, exactly as before records existed.
    FnDclNode *allocmeth = (FnDclNode*)iTypeFindFnField(region, allocMethodName);
    LLVMValueRef allocargs[2];
    uint32_t allocargcnt = 1;
    allocargs[0] = sizeval;
    if (regionAllocTakesRecord(region)) {
        VarDclNode *recparm = (VarDclNode *)nodesGet(((FnSigNode *)itypeGetTypeDcl(allocmeth->vtype))->parms, 1);
        allocargs[allocargcnt++] = genlTypeRecord(gen, reftype->vtexp, recparm->vtype);
    }
    LLVMValueRef malloc = genlFnCallInternal(gen, SimpleDispatch, (INode*)allocmeth, allocargcnt, allocargs, NULL);
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
        LLVMValueRef initval = genlFnCallInternal(gen, SimpleDispatch, (INode*)reginitmeth, 0, NULL, NULL);
        LLVMValueRef regionp = LLVMBuildStructGEP2(gen->builder, reftype->typeinfo->structype, ptrstructype, 0, "region");
        LLVMBuildStore(gen->builder, initval, regionp);
    }

    // Initialize permission, if it is a locked permission with an init method
    if (perm->tag == StructTag) {
        INode *perminitmeth = iTypeFindFnField(perm, initMethodName);
        if (perminitmeth) {
            LLVMValueRef initval = genlFnCallInternal(gen, SimpleDispatch, (INode*)perminitmeth, 0, NULL, NULL);
            LLVMValueRef permp = LLVMBuildStructGEP2(gen->builder, reftype->typeinfo->structype, ptrstructype, 1, "perm");
            LLVMBuildStore(gen->builder, initval, permp);
        }
    }

    // Initialize value (via copy or init function) and return pointer to it
    LLVMValueRef valuep = LLVMBuildStructGEP2(gen->builder, reftype->typeinfo->structype, ptrstructype, ValueField, ""); // Point to value
    if (reftype->tag == RefTag) {
        LLVMBuildStore(gen->builder, tracedval ? tracedval : genlExpr(gen, allocatenode->vtexp), valuep); // Copy value
    }
    else {
        // Handle array fill via run-time generation
        if (allocatenode->vtexp->tag == ArrayLitTag && ((ArrayNode*)allocatenode->vtexp)->dimens->used > 0) {
            genlAllocFillArray(gen, nbrelems, (ArrayNode*)allocatenode->vtexp, valuep, valuetypllvm);
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

// Release what a variable holds: one owner of an owning reference, single or
// slice, goes away. A tuple is one owner of each owning reference it carries,
// so each is released.
void genlReleaseOwning(GenState *gen, LLVMValueRef val, INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag) {
        RefNode *reftype = (RefNode *)typedcl;
        if (regionIsOwning(reftype->region))
            genlRegionDealias(gen, val, reftype);
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
        // A variable listed itself dies in place: an owning reference, or a
        // tuple or an array whose elements have anything to do as they die
        // (flowScopeDealias). A struct or an enum is listed as its drop's call.
        if ((*nodesp)->tag == VarDclTag) {
            VarDclNode *var = (VarDclNode *)*nodesp;
            genlFinalizeAt(gen, var->llvmvar, var->vtype);
        }
        // Generate function calls that drop/dealias values
        else {
            genlExpr(gen, *nodesp);
        }
    }
}
