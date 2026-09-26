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

// If ref type is struct, release each owning reference its fields hold
void genlDealiasFlds(GenState *gen, LLVMValueRef ref, RefNode *refnode) {
    // A slice's elements are not walked: releasing what each element owns is
    // the same element-granularity work an array of owning references needs.
    if (refnode->tag != RefTag)
        return;
    genlReleaseFlds(gen, ref, refnode->vtexp);
}

// Finalize the value of type 'vtype' at 'valptr' where it sits, as its death
// would, without giving its memory back: the 'finalize' intrinsic. An owning
// reference (or a tuple of them) is released. Anything else runs its type's
// drop -- its own 'final', then each finalizing field's -- and then releases
// the owning references its fields hold: genlRegionDeath's order, less the
// region's 'free'. A type for which itypeNeedsFinal is false generates nothing.
void genlFinalizeAt(GenState *gen, LLVMValueRef valptr, INode *vtype) {
    if (flowIsOwningType(vtype)) {
        genlReleaseOwning(gen, LLVMBuildLoad2(gen->builder, genlType(gen, vtype), valptr, "finalref"), vtype);
        return;
    }
    INode *dropfn = itypeGetDropFnDcl(vtype);
    if (dropfn)
        genlFnCallInternal(gen, SimpleDispatch, dropfn, 1, &valptr, NULL);
    genlReleaseFlds(gen, valptr, vtype);
}

// If the value at 'ref' is a struct, release each owning reference its fields hold.
//
// Not an enum's: its drop releases what its variant's fields own, the common
// fields among them (genlEnumDrop), and an enum's drop always runs before this.
// A variant laid out as a nullable pointer has no struct to reach into: the
// value is its one reference.
void genlReleaseFlds(GenState *gen, LLVMValueRef ref, INode *vtype) {
    StructNode *strnode = (StructNode*)itypeGetTypeDcl(vtype);
    if (strnode->tag != StructTag || (strnode->flags & TraitType))
        return;
    INode **nodesp;
    uint32_t cnt;
    if (strnode->flags & NullablePtr) {
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            RefNode *vartype = (RefNode *)itypeGetTypeDcl(((FieldDclNode *)*nodesp)->vtype);
            if (vartype->tag == RefTag && regionIsOwning(vartype->region))
                genlReleaseOwning(gen, LLVMBuildLoad2(gen->builder, genlType(gen, (INode*)vartype), ref, "nullableref"), (INode*)vartype);
        }
        return;
    }
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        // Resolved, because a field's declared type may be a name standing for
        // the reference type rather than the reference type itself
        RefNode *vartype = (RefNode *)itypeGetTypeDcl(field->vtype);
        if (vartype->tag != RefTag || !regionIsOwning(vartype->region))
            continue;
        // The GEP yields the field's address; the release routine wants the
        // reference the field holds, so load it.
        LLVMValueRef fldptr = LLVMBuildStructGEP2(gen->builder, genlType(gen, (INode*)strnode), ref, field->index, &field->namesym->namestr);
        LLVMValueRef fldref = LLVMBuildLoad2(gen->builder, genlType(gen, (INode*)vartype), fldptr, "fldref");
        genlReleaseOwning(gen, fldref, (INode*)vartype);
    }
}

// A struct or enum value at 'valptr' was copied: each counted reference its drop
// releases gains 'amount' holders (flowHeldCounted), as a tuple's elements do.
// The mirror of that drop: through a field whose own drop releases one, and in
// an enum, through the variant the tag picks, into each counted field it holds.
void genlAliasHeld(GenState *gen, LLVMValueRef valptr, INode *type, long long amount) {
    StructNode *strnode = (StructNode *)itypeGetTypeDcl(type);
    LLVMTypeRef strtype = genlType(gen, (INode*)strnode);
    INode **nodesp;
    uint32_t cnt;
    if (!(strnode->flags & EnumType)) {
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            if (flowHeldCounted(field->vtype))
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
            if (flowIsCountedField(field->vtype)) {
                fldptr = LLVMBuildStructGEP2(gen->builder, vartype, varptr, field->index, "");
                RefNode *reftype = (RefNode *)itypeGetTypeDcl(field->vtype);
                genlRegionAlias(gen, LLVMBuildLoad2(gen->builder, genlType(gen, (INode*)reftype), fldptr, "heldref"), amount, reftype);
            }
            else if (flowHeldCounted(field->vtype)) {
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
// drop -- then the owning references its fields hold, the common fields' among
// them. The tag picks the variant, and a variant with nothing to do has no case.
// The nullable-pointer layout has no tag: the value is the one variant's
// reference, and null is the empty variant, which has nothing to do.
void genlEnumDrop(GenState *gen, FnDclNode *fnnode) {
    StructNode *enumnode = (StructNode*)fnnode->dclinfo.owner;
    LLVMValueRef selfptr = LLVMGetParam(gen->fn, 0);
    // No expression gives its calls a place, so each is placed at the enum,
    // where the drop was made (a call with none fails verification in debug)
    if (!gen->opt->release) {
        LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(gen->context,
            fnnode->linenbr, (unsigned)(fnnode->srcp - fnnode->linep), LLVMGetSubprogram(gen->fn), NULL);
        LLVMSetCurrentDebugLocation(gen->builder, LLVMMetadataAsValue(gen->context, loc));
    }
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
    // A function returning nothing returns the empty value, as a 'return' does
    LLVMPositionBuilderAtEnd(gen->builder, doneblk);
    LLVMBuildRet(gen->builder, LLVMGetUndef(gen->emptyStructType));
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

// The value an owning reference points at is dead: finalize it, release what
// its fields own, then give the memory back through the region's 'free', where
// it has one. The finalizer is the type's drop: its own 'final', then each
// field's that has one (structSetDropFn), as a value on the stack is
// finalized. The owning references its fields hold are not in that drop, so
// releasing them after it releases nothing twice. With 'paths', the value or a
// part of it was moved out, and it dies hollow (genlHollowDeath) instead.
static void genlRegionDeath(GenState *gen, LLVMValueRef valptr, RefNode *refnode, MovedPath *paths, int npaths, int depth) {
    if (paths) {
        genlHollowDeath(gen, valptr, refnode, paths, npaths, depth);
        return;
    }
    if (refnode->tag == RefTag) {
        INode *dropfn = itypeGetDropFnDcl(refnode->vtexp);
        if (dropfn)
            genlFnCallInternal(gen, SimpleDispatch, dropfn, 1, &valptr, NULL);
    }
    genlDealiasFlds(gen, valptr, refnode);
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
    FnDclNode *dealiasmeth = regionMethod(refnode->region, dealiasMethodName);
    if (dealiasmeth == NULL && !regionIsMove(refnode->region))
        return;
    LLVMValueRef valptr = genlRefPtr(gen, ref, refnode);
    if (dealiasmeth == NULL) {
        genlRegionDeath(gen, valptr, refnode, paths, npaths, depth);
        return;
    }
    LLVMValueRef last = genlRegionCall(gen, dealiasmeth, genlRegionHeader(gen, valptr, refnode));
    LLVMBasicBlockRef nofree = genlInsertBlock(gen, "nofree");
    LLVMBasicBlockRef dofree = genlInsertBlock(gen, "free");
    LLVMBuildCondBr(gen->builder, last, dofree, nofree);
    LLVMPositionBuilderAtEnd(gen->builder, dofree);
    genlRegionDeath(gen, valptr, refnode, paths, npaths, depth);
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
// an array element -- is left whole to what moved, as a death would release
// none of it either.
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
// never walked, as a death walks none of them.
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

    // Do region allocation (using its alloc method) and then bitcast to multi-layered-struct ptr
    FnDclNode *allocmeth = (FnDclNode*)iTypeFindFnField(region, allocMethodName);
    LLVMValueRef malloc = genlFnCallInternal(gen, SimpleDispatch, (INode*)allocmeth, 1, &sizeval, NULL);
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
        LLVMBuildStore(gen->builder, genlExpr(gen, allocatenode->vtexp), valuep); // Copy value
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
        // Hack for dealias on local variables holding region-owning reference
        if ((*nodesp)->tag == VarDclTag) {
            VarDclNode *var = (VarDclNode *)*nodesp;
            genlReleaseOwning(gen, LLVMBuildLoad2(gen->builder, genlType(gen, var->vtype), var->llvmvar, "allocref"), var->vtype);
        }
        // Generate function calls that drop/dealias values
        else {
            genlExpr(gen, *nodesp);
        }
    }
}
