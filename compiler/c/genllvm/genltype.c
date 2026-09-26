/** Type generation via LLVM
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

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Generate the thunk that fills a vtable slot a folded method satisfies.
//
// Through a virtual reference the concrete type is erased: the call site holds
// an object pointer and a vtable and cannot know that this type folded the
// method through a field, so the shift from the object to that field has to
// happen behind the slot. The thunk is a function of the slot's own type whose
// body shifts the receiver one hop per field on the path -- an address for a
// field held by value, a load for one held through a reference or pointer --
// and tail-calls the method. It is a method in everything but name and
// namespace: nothing in the language can name it, and no slot a declared method
// fills has one.
static LLVMValueRef genlVtableThunk(GenState *gen, Vtable *vtable, VtableImpl *impl, unsigned int pos,
        FnDclNode *meth, Nodes *path) {
    char symbol[2048];
    FnDclNode *slot = (FnDclNode*)nodesGet(vtable->methfld, pos);
    LLVMValueRef fn = LLVMAddFunction(gen->module, nameVtableThunk(symbol, impl->structdcl, vtable->trait, slot->namesym),
        genlVtableSlotFnType(gen, slot));
    genlLinkage(fn, NULL, GenlDefined);
    genlComdat(gen, fn);

    // Its own builder: a vtable is built while some other function may be
    LLVMBuilderRef svbuilder = gen->builder;
    gen->builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(gen->builder, LLVMAppendBasicBlockInContext(gen->context, fn, "entry"));

    // The receiver arrives erased; shift it to the field the method was folded
    // through. 'recvtype' is the Cone type 'recv' points at.
    INode *recvtype = impl->structdcl;
    LLVMValueRef recv = LLVMBuildBitCast(gen->builder, LLVMGetParam(fn, 0),
        LLVMPointerType(genlType(gen, recvtype), 0), "");
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(path, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        recv = LLVMBuildStructGEP2(gen->builder, genlType(gen, recvtype), recv, field->index, &field->namesym->namestr);
        INode *fldtype = itypeGetTypeDcl(field->vtype);
        if (fldtype->tag == RefTag || fldtype->tag == PtrTag) {
            recv = LLVMBuildLoad2(gen->builder, genlType(gen, fldtype), recv, "");
            recvtype = genlPointee(fldtype);
        }
        else
            recvtype = fldtype;
    }

    // The method takes its self as it declared it: a pointer, or the value
    if (meth->llvmvar == NULL)
        genlGloFnName(gen, meth);
    LLVMTypeRef selftype = LLVMTypeOf(LLVMGetParam(meth->llvmvar, 0));
    if (LLVMGetTypeKind(selftype) == LLVMPointerTypeKind)
        recv = LLVMBuildBitCast(gen->builder, recv, selftype, "");
    else
        recv = LLVMBuildLoad2(gen->builder, genlType(gen, recvtype), recv, "");

    unsigned int argcnt = LLVMCountParams(fn);
    LLVMValueRef *args = (LLVMValueRef *)memAllocBlk(argcnt * sizeof(LLVMValueRef));
    args[0] = recv;
    unsigned int argi;
    for (argi = 1; argi < argcnt; ++argi)
        args[argi] = LLVMGetParam(fn, argi);
    LLVMValueRef call = LLVMBuildCall2(gen->builder, genlType(gen, meth->vtype), meth->llvmvar, args, argcnt, "");
    LLVMSetTailCall(call, 1);
    LLVMBuildRet(gen->builder, call);

    LLVMDisposeBuilder(gen->builder);
    gen->builder = svbuilder;
    return fn;
}

// Generate a specific vtable value for some struct
void genlVtableImpl(GenState *gen, Vtable *vtable, VtableImpl *impl, LLVMTypeRef vtableRef) {
    // Ensure the struct has been "built", as we need to point to its fields and methods
    LLVMTypeRef structRef = genlType(gen, impl->structdcl);

    // Build structure containing vtable info
    LLVMValueRef implRef = LLVMGetUndef(vtableRef);
    INode **nodesp;
    uint32_t cnt;
    unsigned int pos = 0;
    for (nodesFor(impl->methfld, cnt, nodesp)) {
        LLVMValueRef val;
        if ((*nodesp)->tag == FieldDclTag) {
            // Calculate byte offset of the field
            FieldDclNode *fld = (FieldDclNode *)*nodesp;
            unsigned long long offset = LLVMOffsetOfElement(gen->datalayout, structRef, fld->index);
            val = LLVMConstInt(LLVMInt32TypeInContext(gen->context), offset, 0);
        }
        else {
            // Pointer to method. Recast so parameter types match later on.
            //
            // The symbol is asked for rather than assumed. A vtable is built the
            // first time a type mentioning it is generated, which happens while
            // the module's symbols are still being declared: a function whose
            // signature names '&<Shape' builds Shape's vtable, and any
            // implementer declared later in the file has no symbol yet. Asking
            // is idempotent, so a method already declared is untouched.
            FnDclNode *meth = (FnDclNode *)*nodesp;
            if (meth->llvmvar == NULL)
                genlGloFnName(gen, meth);
            LLVMTypeRef newfntyp = LLVMStructGetTypeAtIndex(vtableRef, pos);
            // A slot a folded method fills holds a thunk that shifts the
            // receiver to the field the method was folded through
            Nodes *path = impl->foldpaths ? (Nodes*)nodesGet(impl->foldpaths, pos) : NULL;
            if (path)
                val = genlVtableThunk(gen, vtable, impl, pos, meth, path);
            else
                val = LLVMBuildBitCast(gen->builder, meth->llvmvar, newfntyp, "");
        }
        implRef = LLVMBuildInsertValue(gen->builder, implRef, val, pos++, "vtable entry");
    }

    // Create and initialize global variable to hold vtable info
    char symbol[2048];
    impl->llvmvtablep = LLVMAddGlobal(gen->module, vtableRef, nameVtableImpl(symbol, impl->structdcl, vtable->trait));
    LLVMSetGlobalConstant(impl->llvmvtablep, 1);
    // Every object that coerces this type to the trait builds this vtable, and
    // pattern matching compares its address: shared in a described build, so
    // one copy survives the link (genlVtableDefinition)
    genlLinkage(impl->llvmvtablep, NULL, genlVtableDefinition(gen));
    genlComdat(gen, impl->llvmvtablep);
    LLVMSetInitializer(impl->llvmvtablep, implRef);
}

// The function type of the vtable slot a method fills. A self parameter that
// is a reference is re-cast into *u8, so the one slot takes every implementer.
LLVMTypeRef genlVtableSlotFnType(GenState *gen, FnDclNode *meth) {
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
    LLVMTypeRef *parm = param_types;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        assert((*nodesp)->tag == VarDclTag);
        if (cnt == fnsig->parms->used && iexpGetTypeDcl(*nodesp)->tag == RefTag)
            *parm++ = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
        else
            *parm++ = genlType(gen, ((IExpNode *)*nodesp)->vtype);
    }
    return LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
}

// Generate a vtable type
void genlVtable(GenState *gen, Vtable *vtable) {
    // Name the vtable and the virtual reference type, and publish both, before
    // any slot is typed. A slot's type comes from its method's signature, which
    // may name a virtual reference to this same trait ('fn cmp(self &, o &<Self)'),
    // and that is the type being built here. Published, it is answered with a
    // named struct whose body is filled in below, as a struct that points to
    // itself is; unpublished, it would start this vtable again without end.
    // The virtual reference type takes the vtable's name, which LLVM uniquifies
    // with a suffix. It is a fat pointer:
    // - a pointer to the object (for now *u8 - which we will recast later)
    // - a pointer to the vtable
    char vtablename[2048];
    nameVtable(vtablename, vtable->trait);
    LLVMTypeRef vtableRef = LLVMStructCreateNamed(gen->context, vtablename);
    LLVMTypeRef vreffields[2];
    vreffields[0] = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
    vreffields[1] = LLVMPointerType(vtableRef, 0);
    LLVMTypeRef virtref = LLVMStructCreateNamed(gen->context, vtablename);
    LLVMStructSetBody(virtref, vreffields, 2, 0);
    vtable->llvmvtable = vtableRef;
    vtable->llvmreftype = virtref;

    uint32_t fieldcnt = vtable->methfld->used;
    LLVMTypeRef *field_types = (LLVMTypeRef *)memAllocBlk(fieldcnt * sizeof(LLVMTypeRef));
    LLVMTypeRef *field_type_ptr = field_types;

    // Declare vtable's fields
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(vtable->methfld, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag)
            // A pointer to the method's function
            *field_type_ptr++ = LLVMPointerType(genlVtableSlotFnType(gen, (FnDclNode *)*nodesp), 0);
        else
            // All virtual fields are 32-bit offsets into the object
            *field_type_ptr++ = LLVMInt32TypeInContext(gen->context);
    }

    // Fill in the vtable type's body
    if (fieldcnt > 0)
        LLVMStructSetBody(vtableRef, field_types, fieldcnt, 0);

    // Build all the vtable globals that implement the vtable
    // as well as an array pointing to all these vtables
    LLVMValueRef *vtables = (LLVMValueRef *)memAllocBlk(vtable->impl->used * sizeof(LLVMValueRef *));
    LLVMValueRef *vtablesp = vtables;
    for (nodesFor(vtable->impl, cnt, nodesp)) {
        genlVtableImpl(gen, vtable, (VtableImpl*)*nodesp, vtableRef);
        *vtablesp++ = ((VtableImpl*)*nodesp)->llvmvtablep;
    }
    LLVMValueRef vtablelist = LLVMConstArray(LLVMPointerType(vtableRef, 0), vtables, vtable->impl->used);
    char listsymbol[2048];
    vtable->llvmvtables = LLVMAddGlobal(gen->module, LLVMTypeOf(vtablelist), nameVtableList(listsymbol, vtable->trait));
    LLVMSetGlobalConstant(vtable->llvmvtables, 1);
    // The list holds the implementers this compile saw, so it is one per
    // compilation unit and internal in a package compile too
    genlLinkage(vtable->llvmvtables, NULL, GenlDefined);
    genlComdat(gen, vtable->llvmvtables);
    LLVMSetInitializer(vtable->llvmvtables, vtablelist);
}

// Generate the fields for a struct and optionally add padding bytes
LLVMTypeRef genlStructFields(GenState *gen, LLVMTypeRef structype, StructNode *strnode, unsigned int padding) {
    // A type declared @opaque names no fields, so it stays an opaque LLVM struct
    // and may only be pointed at. Every other type here has a layout, including
    // a trait: a trait carries OpaqueType because it has no size as a *value*,
    // which is a different statement from having no fields. Its own fields are
    // known, and they are a prefix of every implementer's, which is what lets a
    // reference to a trait reach the fields the trait declares.
    if (strnode->flags & DeclaredOpaque)
        return structype;

    // Empty struct (void)
    uint32_t fieldcnt = strnode->fields.used;
    if (fieldcnt == 0 && padding == 0) {
        LLVMStructSetBody(structype, NULL, 0, 0);
        return structype;
    }

    // Add struct's fields (body) to type
    INode **nodesp;
    uint32_t cnt;
    LLVMTypeRef *field_types = (LLVMTypeRef *)memAllocBlk(fieldcnt * sizeof(LLVMTypeRef));
    LLVMTypeRef *field_type_ptr = field_types;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        *field_type_ptr++ = genlType(gen, ((FieldDclNode *)*nodesp)->vtype);
    }
    if (padding > 0) {
        *field_type_ptr++ = LLVMArrayType(LLVMInt8TypeInContext(gen->context), padding);
        ++fieldcnt;
    }
    LLVMStructSetBody(structype, field_types, fieldcnt, 0);

    return structype;
}

// For a tagged enum (only do once, if needed):
// - Optimize optional references/pointers to use 0 for lack of pointer
//
// The discriminant's width is not decided here. It follows the largest tag VALUE
// rather than the variant count, which is not 'derived->used' once a tag value may
// be pinned, so type check settles it where the pinned values and any declared
// integer type are both known -- structSetTagWidth.
//
// Each enum decides this for its own set. An extension holds copies of its base's
// variants rather than the base's own declarations, so whatever it adds cannot
// change the base's answer: an Option-shaped base stays a bare pointer, and its
// extension, with a third variant for which there is no pointer to be, is tagged.
void genlSetupTaggedTrait(GenState *gen, StructNode *base) {
    // Set optimization flag if we have a nullable pointer variant types
    if (base->flags & SameSize && base->derived->used == 2) {
        // Look for 2 variants, one with one field (enum) and one with two
        StructNode *nonenode = (StructNode*)nodesGet(base->derived, 0);
        StructNode *somenode = (StructNode*)nodesGet(base->derived, 1);
        if (nonenode->fields.used == 2 && somenode->fields.used == 1) {
            StructNode *tempnode = nonenode;
            nonenode = somenode;
            somenode = tempnode;
        }
        else if (!(nonenode->fields.used == 1 && somenode->fields.used == 2))
            nonenode = NULL;

        // If some derived node's second field is a pointer, mark trait and derived's
        // with optimization flag
        if (nonenode) {
            FieldDclNode *fld = (FieldDclNode*)nodelistGet(&somenode->fields, 1);
            INode *fldtype = itypeGetTypeDcl(fld->vtype);
            if (fldtype->tag == PtrTag || fldtype->tag == RefTag
                || fldtype->tag == VirtRefTag || fldtype->tag == ArrayRefTag) {

                // Yes, we have a nullable pointer, set flags to say so on trait & derived structs
                base->flags |= NullablePtr;
                INode **nodesp;
                uint32_t cnt;
                unsigned long long maxsize = 0;
                base->llvmtype = genlType(gen, fldtype);
                for (nodesFor(base->derived, cnt, nodesp)) {
                    (*nodesp)->flags |= NullablePtr;
                    ((StructNode*)*nodesp)->llvmtype = base->llvmtype;
                }
                return;
            }
        }
    }
}

// Generate samesize trait and all its concrete types, padding as needed. Every
// variant is in exactly one enum's list -- an extension's are copies of its base's
// -- so each is padded to its own enum's largest, and an extension's added variant
// may be larger than anything its base holds.
void genlSameSizeTrait(GenState *gen, StructNode *base) {

    // Generate just "opaque" struct def for all concrete names (trait has already been done)
    // This way any recursive types in fields will be able to latch on to these typerefs, if needed
    // Later we will attach fields to the opaque struct defs
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        if (strnode->llvmtype == NULL)
            strnode->llvmtype = LLVMStructCreateNamed(gen->context, &strnode->namesym->namestr);
    }

    // Use throwaway types to determine the sizes of all concrete variants.
    // Remember the largest size, and the most strictly aligned field type of any
    // variant, which the enum's own type must be aligned to.
    unsigned long long maxsize = 0;
    unsigned int maxalign = 1;
    LLVMTypeRef maxaligntype = NULL;
    unsigned long long *sizes = (unsigned long long *)memAllocBlk(base->derived->used * sizeof(unsigned long long));
    unsigned long long *sizesp = sizes;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, "Throwaway");
        genlStructFields(gen, structype, strnode, 0);
        unsigned long long size = LLVMStoreSizeOfType(gen->datalayout, structype);
        *sizesp++ = size;
        if (size > maxsize)
            maxsize = size;
        unsigned int fldcnt = LLVMCountStructElementTypes(structype);
        unsigned int fld;
        for (fld = 0; fld < fldcnt; ++fld) {
            LLVMTypeRef fldtype = LLVMStructGetTypeAtIndex(structype, fld);
            unsigned int align = LLVMABIAlignmentOfType(gen->datalayout, fldtype);
            if (align > maxalign) {
                maxalign = align;
                maxaligntype = fldtype;
            }
        }
    }
    // Every variant is padded to one size, which must be a multiple of the most
    // strictly aligned variant's alignment. Otherwise a variant padded to the
    // largest's size is rounded up past it by its own alignment, and the variants
    // are not the same size after all.
    maxsize = (maxsize + maxalign - 1) / maxalign * maxalign;

    // Now add fields + padding for all variants, so all end up the same max size
    sizesp = sizes;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        unsigned long long size = *sizesp++;
        // Only a struct has a body to attach. A variant the nullable-pointer
        // layout gave a bare pointer to never reaches here, because genlType
        // answers before calling this.
        if (LLVMGetTypeKind(strnode->llvmtype) == LLVMStructTypeKind && LLVMIsOpaqueStruct(strnode->llvmtype))
            genlStructFields(gen, strnode->llvmtype, strnode, (unsigned int)(maxsize - size));
    }

    // The enum's own type: its own fields (the discriminant and any common fields,
    // which begin every variant at the same offsets), then bytes out to the padded
    // size, then a zero-length array carrying the strictest variant alignment.
    //
    // The enum is loaded, stored and passed as a first-class LLVM value, and LLVM
    // does not preserve a first-class aggregate's padding bytes. So its type may
    // have no padding where any variant has a field: copying a variant's fields
    // would leave another variant's field in a hole of that layout and lose it
    // (a Bool at byte 1 beside a variant whose i32 starts at byte 4). Bytes have
    // no holes, and they reinterpret nothing, where another variant's scalar
    // types would.
    uint32_t ownfields = base->fields.used;
    LLVMTypeRef *basetypes = (LLVMTypeRef *)memAllocBlk((ownfields + 2) * sizeof(LLVMTypeRef));
    uint32_t basecnt = 0;
    for (nodelistFor(&base->fields, cnt, nodesp))
        basetypes[basecnt++] = genlType(gen, ((FieldDclNode *)*nodesp)->vtype);
    unsigned long long ownend = 0;
    if (basecnt > 0) {
        LLVMTypeRef owntype = LLVMStructTypeInContext(gen->context, basetypes, basecnt, 0);
        ownend = LLVMOffsetOfElement(gen->datalayout, owntype, basecnt - 1)
            + LLVMStoreSizeOfType(gen->datalayout, basetypes[basecnt - 1]);
    }
    if (maxsize > ownend)
        basetypes[basecnt++] = LLVMArrayType(LLVMInt8TypeInContext(gen->context), (unsigned int)(maxsize - ownend));
    LLVMTypeRef sofar = LLVMStructTypeInContext(gen->context, basetypes, basecnt, 0);
    if (maxaligntype && LLVMABIAlignmentOfType(gen->datalayout, sofar) < maxalign)
        basetypes[basecnt++] = LLVMArrayType(maxaligntype, 0);
    LLVMStructSetBody(base->llvmtype, basetypes, basecnt, 0);
}

// Generate a struct with no fields (useful for void, etc.)
LLVMTypeRef genlEmptyStruct(GenState* gen) {
    LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, "void");
    LLVMStructSetBody(structype, NULL, 0, 0);
    return structype;
}

// Generate a LLVMTypeRef from a basic type definition node
LLVMTypeRef _genlType(GenState *gen, char *name, INode *typ) {
    switch (typ->tag) {
    case IntNbrTag: case UintNbrTag:
    {
        switch (((NbrNode*)typ)->bits) {
        case 1: return LLVMInt1TypeInContext(gen->context);
        case 8: return LLVMInt8TypeInContext(gen->context);
        case 16: return LLVMInt16TypeInContext(gen->context);
        case 32: return LLVMInt32TypeInContext(gen->context);
        case 64: return LLVMInt64TypeInContext(gen->context);
        }
    }
    case FloatNbrTag:
    {
        switch (((NbrNode*)typ)->bits) {
        case 32: return LLVMFloatTypeInContext(gen->context);
        case 64: return LLVMDoubleTypeInContext(gen->context);
        }
    }

    case VoidTag:
        return gen->emptyStructType;

    case PtrTag:
    {
        LLVMTypeRef vtexp = genlType(gen, ((StarNode *)typ)->vtexp);
        return LLVMPointerType(vtexp, 0);
    }

    case RefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        LLVMTypeRef vtexp = genlType(gen, refnode->vtexp);
        return LLVMPointerType(vtexp, 0);
    }

    case VirtRefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        StructNode *trait = (StructNode*)itypeGetTypeDcl(refnode->vtexp);
        if (trait->vtable->llvmreftype == NULL)
            genlVtable(gen, trait->vtable);
        return trait->vtable->llvmreftype;
    }

    case ArrayRefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        LLVMTypeRef elemtypes[2];
        LLVMTypeRef vtexp = genlType(gen, refnode->vtexp);
        elemtypes[0] = LLVMPointerType(vtexp, 0);
        elemtypes[1] = _genlType(gen, "", (INode*)usizeType);
        return LLVMStructTypeInContext(gen->context, elemtypes, 2, 0);
    }

    case FnSigTag:
    {
        // Build typeref from function signature
        FnSigNode *fnsig = (FnSigNode*)typ;
        LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
        LLVMTypeRef *parm = param_types;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(fnsig->parms, cnt, nodesp)) {
            assert((*nodesp)->tag == VarDclTag);
            *parm++ = genlType(gen, ((IExpNode *)*nodesp)->vtype);
        }
        return LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
    }

    case PermTag:
        return gen->emptyStructType;

    case StructTag:
    {
        StructNode *strnode = (StructNode *)typ;
        if (strnode->llvmtype)
            return strnode->llvmtype;

        // If this struct is (or has) a base trait, do special handling (if needed)       
        StructNode *base = structGetBaseTrait((StructNode*)typ);
        if (base && base->llvmtype == NULL) {
            if (base->flags & HasTagField) {
                genlSetupTaggedTrait(gen, base);
                // The nullable-pointer optimization has already given the base
                // and every variant the bare pointer as its LLVM type, and there
                // is deliberately no struct: a null pointer is the empty variant.
                // Take that answer rather than overwriting it below.
                if (base->flags & NullablePtr)
                    return strnode->llvmtype;
            }

            base->llvmtype = LLVMStructCreateNamed(gen->context, &base->namesym->namestr);
            if (base->flags & SameSize) {
                // This will gen trait + all its concrete variant types
                genlSameSizeTrait(gen, base);
                assert(strnode->llvmtype);
                return strnode->llvmtype;
            }
            genlStructFields(gen, base->llvmtype, base, 0);
        }

        strnode->llvmtype = LLVMStructCreateNamed(gen->context, &strnode->namesym->namestr);
        return genlStructFields(gen, strnode->llvmtype, strnode, 0);
    }

    case EnumTag:
    {
        switch (((EnumNode*)typ)->bytes) {
        case 1: return LLVMInt8TypeInContext(gen->context);
        case 2: return LLVMInt16TypeInContext(gen->context);
        case 4: return LLVMInt32TypeInContext(gen->context);
        case 8: return LLVMInt64TypeInContext(gen->context);
        }
    }

    case TTupleTag:
    {
        // Build struct typeref
        TupleNode *tuple = (TupleNode*)typ;
        INode **nodesp;
        uint32_t cnt;
        uint32_t propcount = tuple->elems->used;
        LLVMTypeRef *typerefs = (LLVMTypeRef *)memAllocBlk(propcount * sizeof(LLVMTypeRef));
        LLVMTypeRef *typerefp = typerefs;
        for (nodesFor(tuple->elems, cnt, nodesp)) {
            *typerefp++ = genlType(gen, *nodesp);
        }
        return LLVMStructTypeInContext(gen->context, typerefs, propcount, 0);
    }

    // Build out (potentially) multi-dimensional array type
    case ArrayTag:
    {
        ArrayNode *anode = (ArrayNode*)typ;
        uint32_t cnt = anode->dimens->used;
        INode **nodesp = &nodesGet(anode->dimens, cnt - 1);
        LLVMTypeRef array = genlType(gen, arrayElemType((INode*)anode)); // Start with element type
        while (cnt--) {
            INode *dimnode = *nodesp--;
            assert(dimnode->tag == ULitTag);
            array = LLVMArrayType(array, (unsigned int)((ULitNode*)dimnode)->uintlit); // Build nested arrays from inside-out
        }
        return array;
    }

    case AliasDclTag:
        return genlType(gen, ((AliasDclNode *)typ)->target);

    default:
        errorUnreachable(typ, "a type code generation has no case for");
        return NULL;
    }
}

// Generate a type value
LLVMTypeRef genlType(GenState *gen, INode *typ) {
    char *name = "";
    INode *dcltype = itypeGetTypeDcl(typ);
    if (isNamedNode(dcltype)) {
        // with vtype name use, we can memoize type value and give it a name
        INsTypeNode *dclnode = (INsTypeNode*)dcltype;
        if (dclnode->llvmtype)
            return dclnode->llvmtype;

        // Note: processing of a type's methods/functions happens elsewhere
        LLVMTypeRef typeref = dclnode->llvmtype = _genlType(gen, &inodeGetName(dcltype)->namestr, (INode*)dclnode);
        return typeref;
    }
    else if (dcltype->tag == RefTag || dcltype->tag == ArrayRefTag || dcltype->tag == VirtRefTag) {
        RefNode *reftype = (RefNode*)dcltype;
        if (reftype->typeinfo == NULL)
            return _genlType(gen, "", dcltype);
        if (reftype->typeinfo->llvmtyperef)
            return reftype->typeinfo->llvmtyperef;
        genlRefTypeSetup(gen, reftype);
        return reftype->typeinfo->llvmtyperef = _genlType(gen, "", dcltype);
    }
    else
        return _genlType(gen, "", dcltype);
}

// The Cone type a reference, pointer or slice points at
INode *genlPointee(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    switch (dcltype->tag) {
    case RefTag: case ArrayRefTag: case ArrayDerefTag: case VirtRefTag:
        return ((RefNode *)dcltype)->vtexp;
    case PtrTag:
        return ((StarNode *)dcltype)->vtexp;
    default:
        errorUnreachable(dcltype, "the pointee of a type that points at nothing");
        return NULL;
    }
}

// The LLVM type of the Cone type a reference, pointer or slice points at
LLVMTypeRef genlPointeeType(GenState *gen, INode *type) {
    return genlType(gen, genlPointee(type));
}

// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype) {
    unsigned long long size = LLVMABISizeOfType(gen->datalayout, genlType(gen, vtype));
    return LLVMConstInt(genlType(gen, (INode*)usizeType), size, 0);
}

// Generate LLVM value corresponding to the alignment the target requires of a type
LLVMValueRef genlAlignof(GenState *gen, INode *vtype) {
    unsigned align = LLVMABIAlignmentOfType(gen->datalayout, genlType(gen, vtype));
    return LLVMConstInt(genlType(gen, (INode*)usizeType), align, 0);
}

// Generate unsigned integer whose bits are same size as a pointer
LLVMTypeRef genlUsize(GenState *gen) {
    return (LLVMPointerSize(gen->datalayout) == 4) ? LLVMInt32TypeInContext(gen->context) : LLVMInt64TypeInContext(gen->context);
}
