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
        if (refnode->tuptype)
            return genlType(gen, (INode*)refnode->tuptype);
        else {
            LLVMTypeRef pvtype = genlType(gen, refnode->pvtype);
            return LLVMPointerType(pvtype, 0);
        }
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
            *parm++ = genlType(gen, ((ITypedNode *)*nodesp)->vtype);
        }
        return LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
    }

    case StructTag:
    case AllocTag:
    {
        // Build typeref from struct
        StructNode *strnode = (StructNode*)typ;
        INode **nodesp;
        uint32_t cnt;
        uint32_t propcount = 0;
        for (imethnodesFor(&strnode->methprops, cnt, nodesp)) {
            if ((*nodesp)->tag == VarDclTag)
                ++propcount;
        }
        LLVMTypeRef *prop_types = (LLVMTypeRef *)memAllocBlk(propcount * sizeof(LLVMTypeRef));
        LLVMTypeRef *property = prop_types;
        for (imethnodesFor(&strnode->methprops, cnt, nodesp)) {
            if ((*nodesp)->tag == VarDclTag)
                *property++ = genlType(gen, ((ITypedNode *)*nodesp)->vtype);
        }
        LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, name);
        if (propcount > 0)
            LLVMStructSetBody(structype, prop_types, propcount, 0);
        return structype;
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
        NamedTypeNode *dclnode = (NamedTypeNode*)dcltype;
        if (dclnode->llvmtype)
            return dclnode->llvmtype;

        // Also process the type's methods and functions
        LLVMTypeRef typeref = dclnode->llvmtype = _genlType(gen, &dclnode->namesym->namestr, (INode*)dclnode);
        if (isMethodType(dclnode)) {
            IMethodNode *tnode = (IMethodNode*)dclnode;
            INode **nodesp;
            uint32_t cnt;
            // Declare just method names first, enabling forward references
            for (imethnodesFor(&tnode->methprops, cnt, nodesp)) {
                if ((*nodesp)->tag == FnDclTag)
                    genlGloFnName(gen, (FnDclNode*)*nodesp);
            }
            // Now generate the code for each method
            for (imethnodesFor(&tnode->methprops, cnt, nodesp)) {
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
