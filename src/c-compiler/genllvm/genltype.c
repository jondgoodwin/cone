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

// Generate a LLVMTypeRef for a struct, based on its fields and alignment
LLVMTypeRef _genlStructType(GenState *gen, char *name, StructNode *strnode) {
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

// Calculate max size and TypeRef for all types derived from this base
// to ensure they are all the same size
void genlSameSizeTrait(GenState *gen, StructNode *base) {

    // Calculate the largest size of the struct variants
    LLVMTypeRef baseTypeRef;
    INode **nodesp;
    uint32_t cnt;
    unsigned long long maxsize = 0;
    for (nodesFor(base->derived, cnt, nodesp)) {
        StructNode *strnode = (StructNode *)*nodesp;
        strnode->llvmtype = _genlStructType(gen, "SameSizeTemp", strnode);
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
    case AllocTag:
    {
        // When dealing with same-sized structs based on a trait,
        // we need to prep them to all be the same maxed size
        if (typ->flags & SameSize) {
            StructNode *base = structGetBaseTrait((StructNode*)typ);
            if (!base->llvmtype)
                genlSameSizeTrait(gen, base);
            // For any arbitrary trait in the inheritance hierarchy, use any arbitrary struct type
            if (typ->flags & TraitType)
                return base->llvmtype;
        }
        return _genlStructType(gen, name, (StructNode*)typ);
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
        if (isMethodType(dclnode)) {
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
        return _genlType(gen, "", typ);
}

// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype) {
    unsigned long long size = LLVMABISizeOfType(gen->datalayout, genlType(gen, vtype));
    if (vtype->tag == AllocTag) {
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
