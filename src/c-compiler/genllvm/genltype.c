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
#include <llvm-c/Transforms/Scalar.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Generate a specific vtable value for some struct
void genlVtableImpl(GenState *gen, VtableImpl *impl, LLVMTypeRef vtableRef) {
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
            // Pointer to method. Recast so parameter types match later on
            LLVMTypeRef newfntyp = LLVMStructGetTypeAtIndex(vtableRef, pos);
            val = LLVMBuildBitCast(gen->builder, ((FnDclNode *)*nodesp)->llvmvar, newfntyp, "");
        }
        implRef = LLVMBuildInsertValue(gen->builder, implRef, val, pos++, "vtable entry");
    }

    // Create and initialize global variable to hold vtable info
    impl->llvmvtablep = LLVMAddGlobal(gen->module, vtableRef, impl->name);
    LLVMSetGlobalConstant(impl->llvmvtablep, 1);
    LLVMSetLinkage(impl->llvmvtablep, LLVMLinkOnceAnyLinkage);
    LLVMSetInitializer(impl->llvmvtablep, implRef);
}

// Generate a vtable type
void genlVtable(GenState *gen, Vtable *vtable) {
    uint32_t fieldcnt = vtable->methfld->used;
    LLVMTypeRef *field_types = (LLVMTypeRef *)memAllocBlk(fieldcnt * sizeof(LLVMTypeRef));
    LLVMTypeRef *field_type_ptr = field_types;

    // Declare vtable's fields
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(vtable->methfld, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag) {
            // Generate a pointer to function signature
            // Note: parm types are not specified to avoid LLVM type check errors on self parm
            FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(((FnDclNode *)*nodesp)->vtype);
            LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
            LLVMTypeRef *parm = param_types;
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(fnsig->parms, cnt, nodesp)) {
                assert((*nodesp)->tag == VarDclTag);
                if (cnt == fnsig->parms->used && iexpGetTypeDcl(*nodesp)->tag == RefTag)
                    // Self parameter ref is re-cast into *u8
                    *parm++ = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
                else
                    *parm++ = genlType(gen, ((IExpNode *)*nodesp)->vtype);
            }
            LLVMTypeRef fnsigref = LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
            *field_type_ptr++ = LLVMPointerType(fnsigref, 0);
        }
        else
            // All virtual fields are 32-bit offsets into the object
            *field_type_ptr++ = LLVMInt32TypeInContext(gen->context);
    }

    // Declare the vtable type itself
    LLVMTypeRef vtableRef = LLVMStructCreateNamed(gen->context, vtable->name);
    if (fieldcnt > 0)
        LLVMStructSetBody(vtableRef, field_types, fieldcnt, 0);

    // Build all the vtable globals that implement the vtable
    // as well as an array pointing to all these vtables
    LLVMValueRef *vtables = (LLVMValueRef *)memAllocBlk(vtable->impl->used * sizeof(LLVMValueRef *));
    LLVMValueRef *vtablesp = vtables;
    for (nodesFor(vtable->impl, cnt, nodesp)) {
        genlVtableImpl(gen, (VtableImpl*)*nodesp, vtableRef);
        *vtablesp++ = ((VtableImpl*)*nodesp)->llvmvtablep;
    }
    LLVMValueRef vtablelist = LLVMConstArray(LLVMPointerType(vtableRef, 0), vtables, vtable->impl->used);
    vtable->llvmvtables = LLVMAddGlobal(gen->module, LLVMTypeOf(vtablelist), "vtable-list");
    LLVMSetGlobalConstant(vtable->llvmvtables, 1);
    LLVMSetLinkage(vtable->llvmvtables, LLVMLinkOnceAnyLinkage);
    LLVMSetInitializer(vtable->llvmvtables, vtablelist);

    // Build the virtual reference type for this vtable. It is a fat pointer:
    // - a pointer to the object (for now *u8 - which we will recast later)
    // - a pointer to the vtable
    LLVMTypeRef vreffields[2];
    vreffields[0] = LLVMPointerType(LLVMInt8TypeInContext(gen->context), 0);
    vreffields[1] = LLVMPointerType(vtableRef, 0);
    LLVMTypeRef virtref = LLVMStructCreateNamed(gen->context, vtable->name);
    LLVMStructSetBody(virtref, vreffields, 2, 0);
    vtable->llvmvtable = vtableRef;
    vtable->llvmreftype = virtref;
}

// Generate the fields for a struct and optionally add padding bytes
LLVMTypeRef genlStructFields(GenState *gen, LLVMTypeRef structype, StructNode *strnode, unsigned int padding) {
    if (strnode->flags & OpaqueType)
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

// For tagged base traits (only do once, if needed):
// - Auto-determine size of tag field
// - Optimize optional references/pointers to use 0 for lack of pointer
void genlSetupTaggedTrait(GenState *gen, StructNode *base) {
    if (base->derived->used > 0x100) {
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&base->fields, cnt, nodesp)) {
            if ((*nodesp)->flags & IsTagField) {
                EnumNode *enumnode = (EnumNode *)itypeGetTypeDcl(*nodesp);
                if (base->derived->used > 0x1000000)
                    enumnode->bytes = 4;
                else if (base->derived->used > 0x10000)
                    enumnode->bytes = 3;
                else if (base->derived->used > 0x100)
                    enumnode->bytes = 2;
            }
        }
    }

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

// Generate samesize trait and all its concrete types, padding as needed
void genlSameSizeTrait(GenState *gen, StructNode *base) {
    
    // Generate just "opaque" struct def for all concrete names (trait has already been done)
    // This way any recursive types in fields will be able to latch on to these typerefs, if needed
    // Later we will attach fields to the opaque struct defs
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        strnode->llvmtype = LLVMStructCreateNamed(gen->context, &strnode->namesym->namestr);
    }

    // Use throwaway types to determine the sizes of all concrete variants
    // Remember the largest size
    StructNode *maxStruct = NULL;
    unsigned long long maxsize = 0;
    unsigned long long *sizes = (unsigned long long *)memAllocBlk(base->derived->used * sizeof(unsigned long long));
    unsigned long long *sizesp = sizes;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, "Throwaway");
        genlStructFields(gen, structype, strnode, 0);
        unsigned long long size = LLVMStoreSizeOfType(gen->datalayout, structype);
        *sizesp++ = size;
        if (size > maxsize) {
            maxsize = size;
            maxStruct = strnode;
        }
    }

    // Now add fields + padding for all variants, so all end up the same max size
    sizesp = sizes;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        genlStructFields(gen, strnode->llvmtype, strnode, (unsigned int)(maxsize - *sizesp++));
    }
    
    // basetrait also needs fields
    if (maxStruct)
        genlStructFields(gen, base->llvmtype, maxStruct, 0);
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
            if (base->flags & HasTagField)
                genlSetupTaggedTrait(gen, base);

            base->llvmtype = LLVMStructCreateNamed(gen->context, &base->namesym->namestr);
            if (base->flags & SameSize) {
                // This will gen trait + all its concrete variant types
                genlSameSizeTrait(gen, base);
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

    case TypedefTag:
        return genlType(gen, ((TypedefNode *)typ)->typeval);

    default:
        assert(0 && "Invalid vtype to generate");
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
        LLVMTypeRef typeref = dclnode->llvmtype = _genlType(gen, &dclnode->namesym->namestr, (INode*)dclnode);
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

// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype) {
    unsigned long long size = LLVMABISizeOfType(gen->datalayout, genlType(gen, vtype));
    return LLVMConstInt(genlType(gen, (INode*)usizeType), size, 0);
}

// Generate unsigned integer whose bits are same size as a pointer
LLVMTypeRef genlUsize(GenState *gen) {
    return (LLVMPointerSize(gen->datalayout) == 4) ? LLVMInt32TypeInContext(gen->context) : LLVMInt64TypeInContext(gen->context);
}
