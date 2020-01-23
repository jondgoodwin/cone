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

// Generate a LLVMTypeRef for a struct, based on its fields and alignment
LLVMTypeRef genlStructType(GenState *gen, char *name, StructNode *strnode) {
    // Build typeref from struct
    INode **nodesp;
    uint32_t cnt;
    uint32_t fieldcnt = strnode->fields.used;
    LLVMTypeRef *field_types = (LLVMTypeRef *)memAllocBlk(fieldcnt * sizeof(LLVMTypeRef));
    LLVMTypeRef *field_type_ptr = field_types;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        *field_type_ptr++ = genlType(gen, ((FieldDclNode *)*nodesp)->vtype);
    }
    LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, name);
    if (fieldcnt > 0)
        LLVMStructSetBody(structype, field_types, fieldcnt, 0);

    return structype;
}

// Do the advanced processing needed on same-size or tagged base traits
void genlBaseTrait(GenState *gen, StructNode *base) {
    // Only do it once
    if (base->llvmtype || base->derived->used == 0)
        return;

    // Preprocess when base trait has a discriminant/tag field
    if ((base->flags & HasTagField)) {
        // Ensure tag field is large enough to handle number of variants
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

    // When dealing with same-sized structs based on a trait,
    // prep them to all be the same maxed size.
    if (!(base->flags & SameSize))
        return;

    // Calculate the largest size of the struct variants
    LLVMTypeRef baseTypeRef;
    INode **nodesp;
    uint32_t cnt;
    unsigned long long maxsize = 0;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        strnode->llvmtype = genlStructType(gen, &base->namesym->namestr, strnode);
        unsigned long long size = LLVMStoreSizeOfType(gen->datalayout, strnode->llvmtype);
        if (size > maxsize) {
            maxsize = size;
            baseTypeRef = strnode->llvmtype;
        }
    }

    // Now pad all variants, as needed, so all end up the same max size
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        unsigned long long size = LLVMStoreSizeOfType(gen->datalayout, strnode->llvmtype);
        if (maxsize > size) {
            // Append a padding field of the excess number of bytes
            ArrayNode *padtyp = newArrayNode();
            padtyp->size = (uint32_t)(maxsize - size);
            padtyp->elemtype = (INode*)i8Type;
            FieldDclNode *padfld = newFieldDclNode(anonName, (INode*)immPerm);
            padfld->vtype = (INode*)padtyp;
            nodelistAdd(&strnode->fields, (INode*)padfld);
        }
        strnode->llvmtype = NULL;
    }

    base->llvmtype = baseTypeRef;
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
        return LLVMVoidTypeInContext(gen->context);

    case PtrTag:
    {
        LLVMTypeRef pvtype = genlType(gen, ((PtrNode *)typ)->pvtype);
        return LLVMPointerType(pvtype, 0);
    }

    case RefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        LLVMTypeRef pvtype = genlType(gen, refnode->pvtype);
        return LLVMPointerType(pvtype, 0);
    }

    case VirtRefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        StructNode *trait = (StructNode*)itypeGetTypeDcl(refnode->pvtype);
        if (trait->vtable->llvmreftype == NULL)
            genlType(gen, (INode*)trait);
        return trait->vtable->llvmreftype;
    }

    case ArrayRefTag:
    {
        RefNode *refnode = (RefNode*)typ;
        LLVMTypeRef elemtypes[2];
        LLVMTypeRef pvtype = genlType(gen, refnode->pvtype);
        elemtypes[0] = LLVMPointerType(pvtype, 0);
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

    case StructTag:
    case RegionTag:
    {
        StructNode *strnode = (StructNode *)typ;
        // Handle base trait preprocessing for sealed variants
        if ((typ->flags & HasTagField) || (typ->flags & SameSize)) {
            StructNode *base = structGetBaseTrait((StructNode*)typ);
            genlBaseTrait(gen, base);

            // For any arbitrary trait in the inheritance hierarchy, use any arbitrary struct type
            if (!base->llvmtype && (typ->flags & TraitType))
                strnode->llvmtype = base->llvmtype;
        }

        if (strnode->vtable)
            genlVtable(gen, strnode->vtable);

        return strnode->llvmtype? strnode->llvmtype : genlStructType(gen, name, (StructNode*)typ);
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
        TTupleNode *tuple = (TTupleNode*)typ;
        INode **nodesp;
        uint32_t cnt;
        uint32_t propcount = tuple->types->used;
        LLVMTypeRef *typerefs = (LLVMTypeRef *)memAllocBlk(propcount * sizeof(LLVMTypeRef));
        LLVMTypeRef *typerefp = typerefs;
        for (nodesFor(tuple->types, cnt, nodesp)) {
            *typerefp++ = genlType(gen, *nodesp);
        }
        return LLVMStructTypeInContext(gen->context, typerefs, propcount, 0);
    }

    case ArrayTag:
    {
        ArrayNode *anode = (ArrayNode*)typ;
        return LLVMArrayType(genlType(gen, anode->elemtype), anode->size);
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

        // Also process the type's methods and functions
        LLVMTypeRef typeref = dclnode->llvmtype = _genlType(gen, &dclnode->namesym->namestr, (INode*)dclnode);
        if (isMethodType(dclnode) && !(dcltype->tag == StructTag && (dcltype->flags & TraitType))) {
            INsTypeNode *tnode = (INsTypeNode*)dclnode;
            INode **nodesp;
            uint32_t cnt;
            // Declare just method names first, enabling forward references
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
                if ((*nodesp)->tag == FnDclTag)
                    genlGloFnName(gen, (FnDclNode*)*nodesp);
            }
            // Now generate the code for each method
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
                if ((*nodesp)->tag == FnDclTag)
                    genlFn(gen, (FnDclNode*)*nodesp);
            }
        }
        return typeref;
    }
    else
        return _genlType(gen, "", dcltype);
}

// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype) {
    unsigned long long size = LLVMABISizeOfType(gen->datalayout, genlType(gen, vtype));
    if (vtype->tag == RegionTag) {
        if (LLVMPointerSize(gen->datalayout) == 4)
            size = (size + 3) & 0xFFFFFFFC;
        else
            size = (size + 7) & 0xFFFFFFFFFFFFFFF8;
    }
    return LLVMConstInt(genlType(gen, (INode*)usizeType), size, 0);
}

// Generate unsigned integer whose bits are same size as a pointer
LLVMTypeRef genlUsize(GenState *gen) {
    return (LLVMPointerSize(gen->datalayout) == 4) ? LLVMInt32TypeInContext(gen->context) : LLVMInt64TypeInContext(gen->context);
}
