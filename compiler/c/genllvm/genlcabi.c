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
 * - SysV x86-64 (Linux, macOS, the BSDs): the System V classification. A
 *   struct of up to 16 bytes has each of its eightbytes classed INTEGER (any
 *   integer or pointer in it) or SSE (floats only), and crosses in registers
 *   as that many scalars: an integer of the bytes it uses, a double, a float
 *   or two floats packed in a <2 x float>, the shapes clang coerces to. One
 *   eightbyte is one scalar; two are two arguments, or a returned pair. A
 *   larger struct, or one that no longer fits the argument registers left
 *   (six integer, eight SSE, a hidden result pointer taking one), is copied
 *   to the stack ('byval'); a larger return goes through 'sret'.
 * - wasm32 is not built: a struct is still handed to LLVM whole, which is
 *   what the C ABI does only for a struct of one scalar.
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
    CAbiCoerce,     // A struct as one or two scalars of the type(s) the C ABI passes it in
    CAbiIndirect,   // A struct through a pointer to a copy: the caller's, or its result slot
    CAbiByval       // A struct copied to the stack, an argument marked 'byval' (SysV)
} CAbiPass;

typedef struct CAbiArg {
    CAbiPass pass;
    LLVMTypeRef parts[2];       // Coerce: the scalar(s) it crosses as
    unsigned nparts;
    LLVMTypeRef type;           // The struct's own LLVM type
    unsigned long long size;
    unsigned align;             // Byval and SysV sret: the alignment the stack copy or result slot is marked with
} CAbiArg;

// How every value of one signature crosses
typedef struct CAbiPlan {
    CAbiArg ret;
    CAbiArg *parms;
    uint32_t nparms;
} CAbiPlan;

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

// ---- Classifying a struct: SysV x86-64 ---------------------------------------

// A scalar in a struct, by byte offset: what the classification sees of its
// fields once nested structs and arrays are flattened
typedef struct CAbiLeaf {
    unsigned long long off;
    unsigned long long size;
    int sse;                    // A float: else an integer or a pointer
    LLVMTypeRef type;
} CAbiLeaf;

#define CAbiMaxLeaves 32
typedef struct CAbiLeaves {
    CAbiLeaf leaf[CAbiMaxLeaves];
    unsigned cnt;
} CAbiLeaves;

// Flatten LLVM type 't', at byte offset 'base', into scalars. False for what
// the classification puts in memory: a field not at its natural alignment
// (packed), a type that is not an integer, pointer, float, struct or array,
// or too many scalars to be 16 bytes.
static int genlCAbiWalk(GenState *gen, LLVMTypeRef t, unsigned long long base, CAbiLeaves *out) {
    LLVMTypeKind kind = LLVMGetTypeKind(t);
    switch (kind) {
    case LLVMIntegerTypeKind: case LLVMPointerTypeKind:
    case LLVMHalfTypeKind: case LLVMBFloatTypeKind: case LLVMFloatTypeKind: case LLVMDoubleTypeKind: {
        unsigned long long size = LLVMABISizeOfType(gen->datalayout, t);
        if (size == 0)
            return 1;
        if (out->cnt >= CAbiMaxLeaves)
            return 0;
        CAbiLeaf *leaf = &out->leaf[out->cnt++];
        leaf->off = base;
        leaf->size = size;
        leaf->sse = kind != LLVMIntegerTypeKind && kind != LLVMPointerTypeKind;
        leaf->type = t;
        return 1;
    }
    case LLVMStructTypeKind: {
        unsigned cnt = LLVMCountStructElementTypes(t);
        for (unsigned i = 0; i < cnt; ++i) {
            LLVMTypeRef elem = LLVMStructGetTypeAtIndex(t, i);
            unsigned long long off = base + LLVMOffsetOfElement(gen->datalayout, t, i);
            if (off % LLVMABIAlignmentOfType(gen->datalayout, elem) != 0)
                return 0;
            if (!genlCAbiWalk(gen, elem, off, out))
                return 0;
        }
        return 1;
    }
    case LLVMArrayTypeKind: {
        unsigned long long len = LLVMGetArrayLength2(t);
        LLVMTypeRef elem = LLVMGetElementType(t);
        unsigned long long esize = LLVMABISizeOfType(gen->datalayout, elem);
        if (len == 0 || esize == 0)
            return 1;
        if (len > CAbiMaxLeaves)
            return 0;
        for (unsigned long long i = 0; i < len; ++i) {
            if (!genlCAbiWalk(gen, elem, base + i * esize, out))
                return 0;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static int genlCAbiIsKind(CAbiLeaf *leaf, LLVMTypeKind kind) {
    return LLVMGetTypeKind(leaf->type) == kind;
}

// The scalar one eightbyte [lo, hi) of a struct crosses as, of class 'sse' (a
// float, or floats only) or INTEGER. As clang coerces it: a lone scalar as
// itself, two floats packed, anything else an integer of the bytes it uses.
static LLVMTypeRef genlCAbiEightbyte(GenState *gen, CAbiLeaves *lv, unsigned long long lo,
        unsigned long long hi, int sse) {
    LLVMContextRef ctx = gen->context;
    unsigned long long len = hi - lo;
    CAbiLeaf *first = NULL;
    unsigned count = 0, floats = 0, halves = 0;
    for (unsigned i = 0; i < lv->cnt; ++i) {
        CAbiLeaf *leaf = &lv->leaf[i];
        if (leaf->off < lo || leaf->off >= hi)
            continue;
        if (!first)
            first = leaf;
        ++count;
        if (genlCAbiIsKind(leaf, LLVMFloatTypeKind))
            ++floats;
        else if (genlCAbiIsKind(leaf, LLVMHalfTypeKind))
            ++halves;
    }
    if (sse) {
        if (count == 1)
            return first->type;
        if (len == 8 && count == 2 && floats == 2)
            return LLVMVectorType(LLVMFloatTypeInContext(ctx), 2);
        if (len == 8 && count == 4 && halves == 4)
            return LLVMVectorType(LLVMHalfTypeInContext(ctx), 4);
        if (len == 4 && count == 2 && halves == 2)
            return LLVMVectorType(LLVMHalfTypeInContext(ctx), 2);
        if (len == 4)
            return LLVMFloatTypeInContext(ctx);
        if (len == 2)
            return LLVMHalfTypeInContext(ctx);
        return LLVMDoubleTypeInContext(ctx);
    }
    // INTEGER: a lone pointer or integer of 8, 16, 32 or 64 bits that starts
    // the eightbyte, with nothing else in it, as itself
    if (first && first->off == lo) {
        if (genlCAbiIsKind(first, LLVMPointerTypeKind))
            return first->type;
        if (genlCAbiIsKind(first, LLVMIntegerTypeKind)) {
            unsigned bits = LLVMGetIntTypeWidth(first->type);
            if (bits == 64 || ((bits == 8 || bits == 16 || bits == 32) && count == 1))
                return first->type;
        }
    }
    return LLVMIntTypeInContext(ctx, (unsigned)(len * 8));
}

// Classify a struct of type 't' and 'size' bytes (at most 16) as the System V
// ABI does. False for memory. Else its scalars are in arg->parts, and
// *needint and *needsse say how many registers of each kind it takes.
static int genlCAbiClassify(GenState *gen, LLVMTypeRef t, unsigned long long size, CAbiArg *arg,
        unsigned *needint, unsigned *needsse) {
    CAbiLeaves lv;
    lv.cnt = 0;
    if (size > 16 || !genlCAbiWalk(gen, t, 0, &lv))
        return 0;
    int cls[2] = {0, 0};   // 0 none, 1 INTEGER, 2 SSE
    for (unsigned i = 0; i < lv.cnt; ++i) {
        CAbiLeaf *leaf = &lv.leaf[i];
        for (unsigned long long e = leaf->off / 8; e <= (leaf->off + leaf->size - 1) / 8; ++e) {
            if (e > 1)
                return 0;
            if (!leaf->sse)
                cls[e] = 1;
            else if (cls[e] == 0)
                cls[e] = 2;
        }
    }
    if (cls[0] == 0)
        return 0;
    unsigned nparts = (size > 8 && cls[1] != 0) ? 2 : 1;
    *needint = *needsse = 0;
    for (unsigned e = 0; e < nparts; ++e) {
        unsigned long long lo = e * 8ull;
        unsigned long long hi = size < lo + 8 ? size : lo + 8;
        arg->parts[e] = genlCAbiEightbyte(gen, &lv, lo, hi, cls[e] == 2);
        if (cls[e] == 2)
            ++*needsse;
        else
            ++*needint;
    }
    arg->nparts = nparts;
    return 1;
}

// ---- The plan: how each value of a signature crosses -------------------------

// Take the registers a value that crosses as its own LLVM type uses
static void genlCAbiTakeRegs(GenState *gen, LLVMTypeRef t, unsigned *freeint, unsigned *freesse) {
    CAbiLeaves lv;
    lv.cnt = 0;
    if (!genlCAbiWalk(gen, t, 0, &lv))
        return;
    for (unsigned i = 0; i < lv.cnt; ++i) {
        unsigned *regs = lv.leaf[i].sse ? freesse : freeint;
        if (*regs > 0)
            --*regs;
    }
}

// How a value of Cone type 'type' crosses: result or parameter. Only a struct
// is lowered. 'freeint' and 'freesse' are the argument registers still free
// (SysV), which the value takes or, if it no longer fits, spills to memory.
static void genlCAbiPassOne(GenState *gen, INode *type, int isret, CAbiArg *arg,
        unsigned *freeint, unsigned *freesse) {
    memset(arg, 0, sizeof(CAbiArg));
    arg->pass = CAbiDirect;
    int lowered = 0;
    LLVMTypeRef t = genlType(gen, type);
    if (itypeGetTypeDcl(type)->tag == StructTag) {
        // A struct the nullable-pointer optimization made a bare pointer is one
        if (LLVMGetTypeKind(t) == LLVMStructTypeKind && LLVMTypeIsSized(t)) {
            arg->size = LLVMABISizeOfType(gen->datalayout, t);
            lowered = arg->size != 0;
        }
    }
    if (!lowered) {
        if (!isret && gen->cabi == CAbiSysV)
            genlCAbiTakeRegs(gen, t, freeint, freesse);
        return;
    }
    arg->type = t;
    arg->align = LLVMABIAlignmentOfType(gen->datalayout, t);
    switch (gen->cabi) {
    case CAbiWin64:
        if (arg->size == 1 || arg->size == 2 || arg->size == 4 || arg->size == 8) {
            arg->pass = CAbiCoerce;
            arg->nparts = 1;
            arg->parts[0] = LLVMIntTypeInContext(gen->context, (unsigned)(arg->size * 8));
        }
        else
            arg->pass = CAbiIndirect;
        break;
    case CAbiSysV: {
        unsigned needint = 0, needsse = 0;
        int inregs = genlCAbiClassify(gen, t, arg->size, arg, &needint, &needsse);
        if (isret) {
            if (inregs)
                arg->pass = CAbiCoerce;
            else {
                arg->pass = CAbiIndirect;
                if (*freeint > 0)
                    --*freeint;     // the hidden result pointer
            }
            break;
        }
        if (inregs && *freeint >= needint && *freesse >= needsse) {
            arg->pass = CAbiCoerce;
            *freeint -= needint;
            *freesse -= needsse;
        }
        else if (*freeint == 0 && arg->size <= 8 && arg->align <= 8) {
            // No integer register left: an eightbyte or less goes on the
            // stack as the integer of its size, not a copy
            arg->pass = CAbiCoerce;
            arg->nparts = 1;
            arg->parts[0] = LLVMIntTypeInContext(gen->context, (unsigned)(arg->size * 8));
        }
        else {
            arg->pass = CAbiByval;
            if (arg->align < 8)
                arg->align = 8;
        }
        break;
    }
    default:
        break;
    }
}

static void genlCAbiPlan(GenState *gen, FnSigNode *fnsig, CAbiPlan *plan) {
    unsigned freeint = 6, freesse = 8;
    plan->nparms = fnsig->parms->used;
    plan->parms = (CAbiArg *)memAllocBlk((plan->nparms + 1) * sizeof(CAbiArg));
    genlCAbiPassOne(gen, fnsig->rettype, 1, &plan->ret, &freeint, &freesse);
    for (uint32_t i = 0; i < plan->nparms; ++i)
        genlCAbiPassOne(gen, ((IExpNode *)nodesGet(fnsig->parms, i))->vtype, 0, &plan->parms[i], &freeint, &freesse);
}

// How many LLVM parameters a value crosses as
static unsigned genlCAbiLlvmParms(CAbiArg *arg) {
    return arg->pass == CAbiCoerce ? arg->nparts : 1;
}

// The LLVM index (first parameter 1) of the first LLVM parameter of the Cone
// parameter 'parm'; a result slot, if the function has one, is index 1
static unsigned genlCAbiParmIndex(CAbiPlan *plan, uint32_t parm) {
    unsigned index = plan->ret.pass == CAbiIndirect ? 2 : 1;
    for (uint32_t i = 0; i < parm; ++i)
        index += genlCAbiLlvmParms(&plan->parms[i]);
    return index;
}

// The scalar(s) a result crosses as: one, or a pair as a struct
static LLVMTypeRef genlCAbiRetType(GenState *gen, CAbiArg *ret) {
    if (ret->nparts == 2)
        return LLVMStructTypeInContext(gen->context, ret->parts, 2, 0);
    return ret->parts[0];
}

// The LLVM function type of a declared function: its Cone signature, or for a
// function that crosses to C, as the C ABI passes its values
LLVMTypeRef genlFnDclType(GenState *gen, FnDclNode *fndcl) {
    if (!genlIsCAbiFn(fndcl))
        return genlType(gen, fndcl->vtype);
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    CAbiPlan plan;
    genlCAbiPlan(gen, fnsig, &plan);
    LLVMTypeRef ptrtype = LLVMPointerTypeInContext(gen->context, 0);
    LLVMTypeRef *parmtypes = (LLVMTypeRef *)memAllocBlk((plan.nparms * 2 + 2) * sizeof(LLVMTypeRef));
    uint32_t parmcnt = 0;

    LLVMTypeRef rettype;
    switch (plan.ret.pass) {
    case CAbiCoerce: rettype = genlCAbiRetType(gen, &plan.ret); break;
    case CAbiIndirect:
        rettype = LLVMVoidTypeInContext(gen->context);
        parmtypes[parmcnt++] = ptrtype;
        break;
    default: rettype = genlType(gen, fnsig->rettype); break;
    }

    for (uint32_t i = 0; i < plan.nparms; ++i) {
        CAbiArg *arg = &plan.parms[i];
        switch (arg->pass) {
        case CAbiCoerce:
            for (unsigned p = 0; p < arg->nparts; ++p)
                parmtypes[parmcnt++] = arg->parts[p];
            break;
        case CAbiIndirect: case CAbiByval:
            parmtypes[parmcnt++] = ptrtype;
            break;
        default:
            parmtypes[parmcnt++] = genlType(gen, ((IExpNode *)nodesGet(fnsig->parms, i))->vtype);
            break;
        }
    }
    return LLVMFunctionType(rettype, parmtypes, parmcnt, 0);
}

// ---- Marks on a declaration and on each call -----------------------------------

// Put an attribute on a function's declaration ('call' NULL) or on a call to it
static void genlCAbiAttr(LLVMValueRef fn, LLVMValueRef call, unsigned index, LLVMAttributeRef attr) {
    if (call)
        LLVMAddCallSiteAttribute(call, index, attr);
    else
        LLVMAddAttributeAtIndex(fn, index, attr);
}

static LLVMAttributeRef genlCAbiAlign(GenState *gen, unsigned align) {
    unsigned kind = LLVMGetEnumAttributeKindForName("align", 5);
    return LLVMCreateEnumAttribute(gen->context, kind, align);
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

// Mark a C-named function's result slot and the values the C ABI widens or
// copies to the stack, on its declaration ('fn') or on a call to it ('call'):
// the result at LLVM index 0, and each parameter at its position, after the
// result slot if it has one
static void genlCAbiMark(GenState *gen, FnSigNode *fnsig, CAbiPlan *plan, LLVMValueRef fn, LLVMValueRef call) {
    if (plan->ret.pass == CAbiIndirect) {
        genlCAbiAttr(fn, call, 1, genlCAbiSret(gen, fnsig->rettype));
        if (gen->cabi == CAbiSysV)
            genlCAbiAttr(fn, call, 1, genlCAbiAlign(gen, plan->ret.align));
    }
    LLVMAttributeRef ext = genlCAbiExtend(gen, fnsig->rettype);
    if (ext)
        genlCAbiAttr(fn, call, LLVMAttributeReturnIndex, ext);
    for (uint32_t i = 0; i < plan->nparms; ++i) {
        CAbiArg *arg = &plan->parms[i];
        unsigned index = genlCAbiParmIndex(plan, i);
        if (arg->pass == CAbiByval) {
            unsigned kind = LLVMGetEnumAttributeKindForName("byval", 5);
            genlCAbiAttr(fn, call, index, LLVMCreateTypeAttribute(gen->context, kind, arg->type));
            genlCAbiAttr(fn, call, index, genlCAbiAlign(gen, arg->align));
        }
        else if (arg->pass == CAbiDirect) {
            ext = genlCAbiExtend(gen, ((IExpNode *)nodesGet(fnsig->parms, i))->vtype);
            if (ext)
                genlCAbiAttr(fn, call, index, ext);
        }
    }
}

// Mark a just-declared function's result slot, if it returns through one, and
// the values the C ABI widens or copies
void genlCAbiDeclare(GenState *gen, FnDclNode *fndcl, LLVMValueRef fn) {
    if (!genlIsCAbiFn(fndcl))
        return;
    // Marked, so that the copying of large aggregates in memory leaves its
    // convention alone (genlaggcopy.c, which removes the mark)
    if (!gen->opt->gpu)
        LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
            LLVMCreateStringAttribute(gen->context, "cone-cabi", 9, "", 0));
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    CAbiPlan plan;
    genlCAbiPlan(gen, fnsig, &plan);
    genlCAbiMark(gen, fnsig, &plan, fn, NULL);
}

// ---- Moving a struct between its Cone value and the scalars it crosses as ------

// A buffer a struct is spilled to and its scalars read from (or the reverse):
// two eightbytes, aligned for the struct, so a scalar of either is whole in it
static LLVMValueRef genlCAbiBuf(GenState *gen, CAbiArg *arg) {
    LLVMTypeRef i64 = LLVMInt64TypeInContext(gen->context);
    LLVMValueRef slot = genlAlloca(gen, LLVMArrayType2(i64, 2), "cabi");
    LLVMSetAlignment(slot, arg->align < 8 ? 8 : arg->align);
    return slot;
}

// The address of the 'part'th scalar in a buffer
static LLVMValueRef genlCAbiPartPtr(GenState *gen, LLVMValueRef buf, unsigned part) {
    if (part == 0)
        return buf;
    LLVMValueRef off = LLVMConstInt(LLVMInt64TypeInContext(gen->context), 8, 0);
    return LLVMBuildGEP2(gen->builder, LLVMInt8TypeInContext(gen->context), buf, &off, 1, "");
}

// A struct as the scalar(s) it crosses as, in 'out'
static void genlCAbiSplit(GenState *gen, CAbiArg *arg, LLVMValueRef val, LLVMValueRef *out) {
    LLVMValueRef buf = genlCAbiBuf(gen, arg);
    LLVMBuildStore(gen->builder, val, buf);
    for (unsigned p = 0; p < arg->nparts; ++p)
        out[p] = LLVMBuildLoad2(gen->builder, arg->parts[p], genlCAbiPartPtr(gen, buf, p), "");
}

// The struct whose scalars 'in' are
static LLVMValueRef genlCAbiJoin(GenState *gen, CAbiArg *arg, LLVMValueRef *in) {
    LLVMValueRef buf = genlCAbiBuf(gen, arg);
    for (unsigned p = 0; p < arg->nparts; ++p)
        LLVMBuildStore(gen->builder, in[p], genlCAbiPartPtr(gen, buf, p));
    return LLVMBuildLoad2(gen->builder, arg->type, buf, "");
}

// ---- Calls, parameters and returns -----------------------------------------------

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
    CAbiPlan plan;
    genlCAbiPlan(gen, fnsig, &plan);
    LLVMValueRef *cargs = (LLVMValueRef *)memAllocBlk((argcnt * 2 + 2) * sizeof(LLVMValueRef));
    uint32_t cargcnt = 0;

    LLVMTypeRef rettype = genlType(gen, fnsig->rettype);
    LLVMValueRef retslot = NULL;
    if (plan.ret.pass == CAbiIndirect) {
        retslot = genlAlloca(gen, rettype, "cret");
        if (gen->cabi == CAbiSysV && plan.ret.align > LLVMGetAlignment(retslot))
            LLVMSetAlignment(retslot, plan.ret.align);
        cargs[cargcnt++] = retslot;
    }

    for (uint32_t i = 0; i < argcnt; ++i) {
        CAbiArg *arg = &plan.parms[i];
        switch (arg->pass) {
        case CAbiCoerce:
            genlCAbiSplit(gen, arg, args[i], &cargs[cargcnt]);
            cargcnt += arg->nparts;
            break;
        case CAbiIndirect: case CAbiByval: {
            // The copy the callee may change without the caller seeing it
            LLVMValueRef copy = genlAlloca(gen, arg->type, "carg");
            if (arg->pass == CAbiByval)
                LLVMSetAlignment(copy, arg->align);
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
    genlCAbiMark(gen, fnsig, &plan, NULL, call);
    switch (plan.ret.pass) {
    case CAbiIndirect:
        return LLVMBuildLoad2(gen->builder, rettype, retslot, "");
    case CAbiCoerce: {
        LLVMValueRef parts[2];
        if (plan.ret.nparts == 2) {
            parts[0] = LLVMBuildExtractValue(gen->builder, call, 0, "");
            parts[1] = LLVMBuildExtractValue(gen->builder, call, 1, "");
        }
        else
            parts[0] = call;
        return genlCAbiJoin(gen, &plan.ret, parts);
    }
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
    CAbiPlan plan;
    genlCAbiPlan(gen, fnsig, &plan);
    unsigned index = genlCAbiParmIndex(&plan, var->index) - 1;   // LLVMGetParam counts from 0
    CAbiArg *arg = &plan.parms[var->index];
    switch (arg->pass) {
    case CAbiCoerce: {
        LLVMValueRef parts[2];
        for (unsigned p = 0; p < arg->nparts; ++p)
            parts[p] = LLVMGetParam(gen->fn, index + p);
        return genlCAbiJoin(gen, arg, parts);
    }
    case CAbiIndirect: case CAbiByval:
        return LLVMBuildLoad2(gen->builder, genlType(gen, var->vtype), LLVMGetParam(gen->fn, index), "");
    default:
        return LLVMGetParam(gen->fn, index);
    }
}

// Return 'retval', the Cone value the function being generated returns, as its
// caller expects it
void genlFnDclReturn(GenState *gen, FnDclNode *fndcl, LLVMValueRef retval) {
    if (fndcl == NULL || !genlIsCAbiFn(fndcl)) {
        LLVMBuildRet(gen->builder, retval);
        return;
    }
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(fndcl->vtype);
    CAbiPlan plan;
    genlCAbiPlan(gen, fnsig, &plan);
    switch (plan.ret.pass) {
    case CAbiCoerce: {
        LLVMValueRef parts[2];
        genlCAbiSplit(gen, &plan.ret, retval, parts);
        if (plan.ret.nparts == 2) {
            LLVMValueRef pair = LLVMGetUndef(genlCAbiRetType(gen, &plan.ret));
            pair = LLVMBuildInsertValue(gen->builder, pair, parts[0], 0, "");
            pair = LLVMBuildInsertValue(gen->builder, pair, parts[1], 1, "");
            LLVMBuildRet(gen->builder, pair);
        }
        else
            LLVMBuildRet(gen->builder, parts[0]);
        break;
    }
    case CAbiIndirect:
        LLVMBuildStore(gen->builder, retval, LLVMGetParam(gen->fn, 0));
        LLVMBuildRetVoid(gen->builder);
        break;
    default:
        LLVMBuildRet(gen->builder, retval);
        break;
    }
}

