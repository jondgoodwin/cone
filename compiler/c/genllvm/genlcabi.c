/** The C calling convention: how a C-named function's values cross to C
 * @file
 *
 * A C-named function is a binding to C: an 'extern' one C defines, and a body
 * C calls. Its values cross the boundary as the platform's C compiler passes
 * them, not as LLVM would pass the same first-class aggregate: LLVM hands a
 * struct over one register per field, and no C ABI does that. So a struct
 * passed or returned by value is lowered here, the one place that decides
 * how, from the target's C ABI:
 *
 * - Win64 (x86_64 Windows): a struct of 1, 2, 4 or 8 bytes is one integer of
 *   its size, whatever its fields (floats too); any other size is passed as a
 *   pointer to a copy the caller made, and returned through a hidden pointer
 *   to the caller's result slot ('sret'), the first parameter. This is the
 *   shape clang gives the same C declaration for x86_64-pc-windows-msvc.
 * - SysV x86-64 and wasm32 are not built: a struct is still handed to LLVM
 *   whole, which is what either C ABI does only for a struct of one scalar.
 *
 * An integer narrower than C's 'int' keeps its LLVM type but is marked, on the
 * declaration and at each call, with the widening C gives it in its register:
 * a bool, C's 'bool', 'zeroext' on all three; an 8- or 16-bit integer
 * 'signext' or 'zeroext' by its sign on SysV and wasm32, and nothing on Win64.
 *
 * Only a struct is lowered. A slice, a virtual reference, a tuple or an array
 * has no C counterpart, and keeps Cone's own convention: a slice's pointer and
 * length arrive as two arguments, as a C function's '(char *, size_t)'
 * takes them.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "genllvm.h"

#include <string.h>
#include <assert.h>

// How one value crosses a C-named function's boundary
typedef enum CAbiPass {
    CAbiDirect,     // As its own LLVM type
    CAbiInteger,    // A struct as one integer of its size
    CAbiIndirect    // A struct through a pointer to a copy: the caller's, or its result slot
} CAbiPass;

// The platform C ABI a target triple names
int genlCAbiTarget(const char *triple) {
    if (strncmp(triple, "x86_64", 6) == 0 || strncmp(triple, "amd64", 5) == 0) {
        if (strstr(triple, "windows") || strstr(triple, "win32") || strstr(triple, "mingw")
            || strstr(triple, "cygwin"))
            return CAbiWin64;
        return CAbiSysV;
    }
    if (strncmp(triple, "wasm32", 6) == 0)
        return CAbiWasm32;
    return CAbiOther;
}

// Whether a function's values cross as C passes them: a C-named function a
// module owns. A type's method keeps Cone's convention even when '@c' names
// it, since a vtable slot calls it with the method's Cone signature.
int genlIsCAbiFn(FnDclNode *fndcl) {
    INode *owner = fndcl->dclinfo.owner;
    return (fndcl->dclinfo.facts & DclCName) && owner && owner->tag == ModuleTag;
}

// How a value of Cone type 'type' crosses, and its size in bytes
static CAbiPass genlCAbiPass(GenState *gen, INode *type, unsigned long long *size) {
    *size = 0;
    if (itypeGetTypeDcl(type)->tag != StructTag)
        return CAbiDirect;
    LLVMTypeRef llvmtype = genlType(gen, type);
    // A struct the nullable-pointer optimization made a bare pointer is one
    if (LLVMGetTypeKind(llvmtype) != LLVMStructTypeKind || !LLVMTypeIsSized(llvmtype))
        return CAbiDirect;
    *size = LLVMABISizeOfType(gen->datalayout, llvmtype);
    if (*size == 0)
        return CAbiDirect;
    switch (gen->cabi) {
    case CAbiWin64:
        return (*size == 1 || *size == 2 || *size == 4 || *size == 8) ? CAbiInteger : CAbiIndirect;
    default:
        // TODO SysV x86-64: classify each eightbyte INTEGER or SSE, pass a
        // struct of up to 16 bytes in those registers (coerced to i64, double,
        // <2 x float> or a pair of them), a larger one in memory (byval), and
        // return one through 'sret'. TODO wasm32 (clang's basic C ABI): a
        // struct of one scalar as that scalar, any other through a pointer to
        // a copy, and returned through 'sret'.
        return CAbiDirect;
    }
}

// A slot of 'type' in this function's frame, aligned for an integer of its size too
static LLVMValueRef genlCAbiSlot(GenState *gen, LLVMTypeRef type, unsigned long long size, const char *name) {
    LLVMValueRef slot = genlAlloca(gen, type, name);
    unsigned align = LLVMABIAlignmentOfType(gen->datalayout, type);
    if (size > align)
        LLVMSetAlignment(slot, (unsigned)size);
    return slot;
}

// Reinterpret 'val', a value of LLVM type 'from', as LLVM type 'to' of the same
// size, through memory
static LLVMValueRef genlCAbiRecast(GenState *gen, LLVMValueRef val, LLVMTypeRef from, LLVMTypeRef to,
        unsigned long long size) {
    LLVMTypeRef structtype = LLVMGetTypeKind(from) == LLVMStructTypeKind ? from : to;
    LLVMValueRef slot = genlCAbiSlot(gen, structtype, size, "cabi");
    LLVMBuildStore(gen->builder, val, slot);
    return LLVMBuildLoad2(gen->builder, to, slot, "");
}

static LLVMTypeRef genlCAbiInt(GenState *gen, unsigned long long size) {
    return LLVMIntTypeInContext(gen->context, (unsigned)(size * 8));
}

// The LLVM function type of a declared function: its Cone signature, or for a
// function that crosses to C, as the C ABI passes its values
LLVMTypeRef genlFnDclType(GenState *gen, FnDclNode *fndcl) {
    if (!genlIsCAbiFn(fndcl))
        return genlType(gen, fndcl->vtype);
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    LLVMTypeRef ptrtype = LLVMPointerTypeInContext(gen->context, 0);
    LLVMTypeRef *parmtypes = (LLVMTypeRef *)memAllocBlk((fnsig->parms->used + 1) * sizeof(LLVMTypeRef));
    uint32_t parmcnt = 0;
    unsigned long long size;

    LLVMTypeRef rettype;
    switch (genlCAbiPass(gen, fnsig->rettype, &size)) {
    case CAbiInteger: rettype = genlCAbiInt(gen, size); break;
    case CAbiIndirect:
        rettype = LLVMVoidTypeInContext(gen->context);
        parmtypes[parmcnt++] = ptrtype;
        break;
    default: rettype = genlType(gen, fnsig->rettype); break;
    }

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        INode *parmtype = ((IExpNode *)*nodesp)->vtype;
        switch (genlCAbiPass(gen, parmtype, &size)) {
        case CAbiInteger: parmtypes[parmcnt++] = genlCAbiInt(gen, size); break;
        case CAbiIndirect: parmtypes[parmcnt++] = ptrtype; break;
        default: parmtypes[parmcnt++] = genlType(gen, parmtype); break;
        }
    }
    return LLVMFunctionType(rettype, parmtypes, parmcnt, 0);
}

// The 'sret' attribute marking a function's result slot, of the struct it returns
static LLVMAttributeRef genlCAbiSret(GenState *gen, INode *rettype) {
    unsigned kind = LLVMGetEnumAttributeKindForName("sret", 4);
    return LLVMCreateTypeAttribute(gen->context, kind, genlType(gen, rettype));
}

// The attribute widening an integer narrower than C's 'int' in its register,
// as the C ABI widens it, or NULL. C's 'bool' is zero-extended on each C ABI
// built. A char or a short is sign- or zero-extended as its type is signed on
// SysV x86-64 and wasm32, and left as it is on Win64, where the callee widens
// it. clang marks the same C declaration so for each target.
static LLVMAttributeRef genlCAbiExtend(GenState *gen, INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != IntNbrTag && dcl->tag != UintNbrTag)
        return NULL;
    unsigned bits = ((NbrNode*)dcl)->bits;
    if (bits >= 32)
        return NULL;
    switch (gen->cabi) {
    case CAbiWin64:
        if (bits != 1)
            return NULL;
        break;
    case CAbiSysV: case CAbiWasm32:
        break;
    default:
        return NULL;
    }
    const char *name = dcl->tag == IntNbrTag ? "signext" : "zeroext";
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    return LLVMCreateEnumAttribute(gen->context, kind, 0);
}

// Mark a C-named function's scalars widened by the C ABI, on its declaration
// ('fn') or on a call to it ('call'): the result at LLVM index 0, and each
// parameter at its position, after the result slot if it has one
static void genlCAbiMarkExtends(GenState *gen, FnSigNode *fnsig, LLVMValueRef fn, LLVMValueRef call) {
    unsigned long long size;
    unsigned index = genlCAbiPass(gen, fnsig->rettype, &size) == CAbiIndirect ? 2 : 1;
    LLVMAttributeRef ext = genlCAbiExtend(gen, fnsig->rettype);
    if (ext) {
        if (call)
            LLVMAddCallSiteAttribute(call, LLVMAttributeReturnIndex, ext);
        else
            LLVMAddAttributeAtIndex(fn, LLVMAttributeReturnIndex, ext);
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        ext = genlCAbiExtend(gen, ((IExpNode *)*nodesp)->vtype);
        if (ext) {
            if (call)
                LLVMAddCallSiteAttribute(call, index, ext);
            else
                LLVMAddAttributeAtIndex(fn, index, ext);
        }
        ++index;
    }
}

// Mark a just-declared function's result slot, if it returns through one, and
// the scalars the C ABI widens
void genlCAbiDeclare(GenState *gen, FnDclNode *fndcl, LLVMValueRef fn) {
    if (!genlIsCAbiFn(fndcl))
        return;
    // Marked, so that the copying of large aggregates in memory leaves its
    // convention alone (genlaggcopy.c, which removes the mark)
    if (!gen->opt->gpu)
        LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
            LLVMCreateStringAttribute(gen->context, "cone-cabi", 9, "", 0));
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    unsigned long long size;
    if (genlCAbiPass(gen, fnsig->rettype, &size) == CAbiIndirect)
        LLVMAddAttributeAtIndex(fn, 1, genlCAbiSret(gen, fnsig->rettype));
    genlCAbiMarkExtends(gen, fnsig, fn, NULL);
}

// Build the call instruction itself, in the function's calling convention
static LLVMValueRef genlFnDclCallInst(GenState *gen, FnDclNode *fndcl, LLVMTypeRef fntype, LLVMValueRef fn,
        LLVMValueRef *args, uint32_t argcnt) {
    LLVMValueRef call = LLVMBuildCall2(gen->builder, fntype, fn, args, argcnt, "");
    if (fndcl->dclinfo.facts & DclSystemCC)
        LLVMSetInstructionCallConv(call, LLVMX86StdcallCallConv);
    return call;
}

// Call a declared function 'fn' with the Cone values 'args', returning the Cone
// value it returns. A function that crosses to C is handed its arguments, and
// hands back its result, as the C ABI passes them.
LLVMValueRef genlFnDclCall(GenState *gen, FnDclNode *fndcl, LLVMValueRef fn, LLVMValueRef *args, uint32_t argcnt) {
    LLVMTypeRef fntype = genlFnDclType(gen, fndcl);
    if (!genlIsCAbiFn(fndcl))
        return genlFnDclCallInst(gen, fndcl, fntype, fn, args, argcnt);

    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    assert(argcnt == fnsig->parms->used);
    LLVMValueRef *cargs = (LLVMValueRef *)memAllocBlk((argcnt + 1) * sizeof(LLVMValueRef));
    uint32_t cargcnt = 0;
    unsigned long long size;

    LLVMTypeRef rettype = genlType(gen, fnsig->rettype);
    unsigned long long retsize;
    CAbiPass retpass = genlCAbiPass(gen, fnsig->rettype, &retsize);
    LLVMValueRef retslot = NULL;
    if (retpass == CAbiIndirect)
        cargs[cargcnt++] = retslot = genlAlloca(gen, rettype, "cret");

    uint32_t i;
    for (i = 0; i < argcnt; ++i) {
        INode *parmtype = ((IExpNode *)nodesGet(fnsig->parms, i))->vtype;
        LLVMTypeRef argtype = genlType(gen, parmtype);
        switch (genlCAbiPass(gen, parmtype, &size)) {
        case CAbiInteger:
            cargs[cargcnt++] = genlCAbiRecast(gen, args[i], argtype, genlCAbiInt(gen, size), size);
            break;
        case CAbiIndirect: {
            // The copy the callee may change without the caller seeing it
            LLVMValueRef copy = genlAlloca(gen, argtype, "carg");
            LLVMBuildStore(gen->builder, args[i], copy);
            cargs[cargcnt++] = copy;
            break;
        }
        default:
            cargs[cargcnt++] = args[i];
            break;
        }
    }

    LLVMValueRef call = genlFnDclCallInst(gen, fndcl, fntype, fn, cargs, cargcnt);
    genlCAbiMarkExtends(gen, fnsig, NULL, call);
    switch (retpass) {
    case CAbiIndirect:
        LLVMAddCallSiteAttribute(call, 1, genlCAbiSret(gen, fnsig->rettype));
        return LLVMBuildLoad2(gen->builder, rettype, retslot, "");
    case CAbiInteger:
        return genlCAbiRecast(gen, call, genlCAbiInt(gen, retsize), rettype, retsize);
    default:
        return call;
    }
}

// The Cone value of parameter 'var' of the function being generated, as its
// caller passed it
LLVMValueRef genlFnDclParm(GenState *gen, FnDclNode *fndcl, VarDclNode *var) {
    if (!genlIsCAbiFn(fndcl))
        return LLVMGetParam(gen->fn, var->index);
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    unsigned long long size;
    unsigned index = var->index;
    if (genlCAbiPass(gen, fnsig->rettype, &size) == CAbiIndirect)
        ++index;    // after the result slot
    LLVMValueRef parm = LLVMGetParam(gen->fn, index);
    LLVMTypeRef vartype = genlType(gen, var->vtype);
    switch (genlCAbiPass(gen, var->vtype, &size)) {
    case CAbiInteger:
        return genlCAbiRecast(gen, parm, genlCAbiInt(gen, size), vartype, size);
    case CAbiIndirect:
        return LLVMBuildLoad2(gen->builder, vartype, parm, "");
    default:
        return parm;
    }
}

// Return 'retval', the Cone value the function being generated returns, as its
// caller expects it
void genlFnDclReturn(GenState *gen, FnDclNode *fndcl, LLVMValueRef retval) {
    if (fndcl == NULL || !genlIsCAbiFn(fndcl)) {
        LLVMBuildRet(gen->builder, retval);
        return;
    }
    INode *rettype = ((FnSigNode*)itypeGetTypeDcl(fndcl->vtype))->rettype;
    unsigned long long size;
    switch (genlCAbiPass(gen, rettype, &size)) {
    case CAbiInteger:
        LLVMBuildRet(gen->builder, genlCAbiRecast(gen, retval, genlType(gen, rettype), genlCAbiInt(gen, size), size));
        break;
    case CAbiIndirect:
        LLVMBuildStore(gen->builder, retval, LLVMGetParam(gen->fn, 0));
        LLVMBuildRetVoid(gen->builder);
        break;
    default:
        LLVMBuildRet(gen->builder, retval);
        break;
    }
}
