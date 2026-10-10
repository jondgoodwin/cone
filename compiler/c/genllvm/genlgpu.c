/** Compute entry points, and what a kernel's failed checks and slices become
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../shared/error.h"
#include "../shared/memory.h"
#include "../coneopts.h"
#include "genllvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// A compute entry point, 'fn @compute(64) bake(inv Invocation, parts &Array[Part],
// out &mut Array[f32])', is a kernel on SPIR-V's Vulkan form. Its Cone function is
// generated as every function is, internal and inlined; beside it goes the
// kernel itself, an LLVM function of no parameters named as the Cone function
// is, marked the compute shader it is ("hlsl.shader", "hlsl.numthreads", the
// attributes LLVM's SPIR-V backend reads, as Clang's HLSL marks one), which
// fills each parameter from what the dispatch binds and calls it:
//
// - core's Invocation from the invocation's built-ins (GlobalInvocationId,
//   LocalInvocationId, WorkgroupId, LocalInvocationIndex, NumWorkgroups);
// - each slice from a storage buffer, descriptor set 0, binding numbered by
//   its place among the buffer parameters: the slice's pointer is the
//   buffer's first element and its count the buffer's run-time array's
//   length (OpArrayLength, below);
// - each struct taken by value from a small read-only storage buffer holding
//   one value, the next binding.
//
// The binding after the last buffer is the kernel's error buffer, where a
// failed check records itself (genlGpuRecord). It is a parameter of no Cone
// function; gpuwork binds it as one more read-write buffer.
//
// Every call is inlined into the kernel (genlGpuCalls), so once the GPU
// pipeline has run, the whole kernel is one function, and two things are
// settled there that only that can settle (genlGpuEntries): what a failed
// check does, which is to record itself and leave the kernel; and what an
// element of a slice is, an access chain into the buffer or the fixed array
// the slice came from, since SPIR-V's logical addressing has no pointer
// arithmetic.

// The words of a kernel's error buffer, 32-bit each. The first is a count of
// the invocations that failed, added to atomically; the invocation that
// makes it 1 is the first, and only it writes the rest, so they describe one
// failure whole. The buffer is zeroed before the dispatch and read after it.
enum GenlGpuFailWord {
    FailCount,      // how many invocations failed; 0 when none did
    FailKind,       // what failed (GenlGpuFailKind)
    FailFile,       // the source file's id: its name in the module's 'cone.file' list
    FailLine,       // the line
    FailIdX,        // the global invocation id of the first to fail
    FailIdY,
    FailIdZ,
    FailValue,      // the index (an index), the range's end (a slice), else 0
    FailWords
};

// What failed, in an error buffer's FailKind word
enum GenlGpuFailKind {
    GpuFailIndex = 1,   // an index at or past the count
    GpuFailSlice = 2,   // a range not within the count
    GpuFailPanic = 3,   // a panic: 'panic', 'assert', 'unreachable', 'todo'
    GpuFailAlloc = 4    // an allocation answered nothing
};

// LLVM's address space for SPIR-V's StorageBuffer storage class
#define GenlStorageBuffer 11

static LLVMTypeRef genlI32(GenState *gen) {
    return LLVMInt32TypeInContext(gen->context);
}

static LLVMValueRef genlI32Const(GenState *gen, unsigned long long n) {
    return LLVMConstInt(genlI32(gen), n, 0);
}

// An intrinsic's declaration, its overloads given
static LLVMValueRef genlGpuIntrinsic(GenState *gen, const char *name, LLVMTypeRef *overloads, size_t cnt) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    assert(id != 0 && "LLVM's SPIR-V intrinsics are known to the LLVM Cone builds with");
    return LLVMGetIntrinsicDeclaration(gen->module, id, overloads, cnt);
}

static LLVMValueRef genlGpuCall(GenState *gen, LLVMValueRef fn, LLVMValueRef *args, unsigned nargs) {
    return LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(fn), fn, args, nargs, "");
}

// One of the invocation's built-ins, along 'dim' (0, 1 or 2)
static LLVMValueRef genlGpuBuiltin(GenState *gen, const char *name, int dim) {
    LLVMTypeRef i32 = genlI32(gen);
    LLVMValueRef dimarg = genlI32Const(gen, dim);
    return genlGpuCall(gen, genlGpuIntrinsic(gen, name, &i32, 1), &dimarg, 1);
}

// ---- What a failed check calls ----------------------------------------------

// A failed check on a GPU target calls this, declared and never defined: no
// GPU has conestd to call. In a kernel, each call becomes the record of the
// failure in the kernel's error buffer and the kernel's return
// (genlGpuRecord); elsewhere, in a library compiled for a GPU, it stays a
// call to a function the module imports. Its operands: the kind
// (GenlGpuFailKind), the address of the source file's name (genlSrcFileText,
// whose global gives the file's id), the line and the value it reports.
static LLVMValueRef genlGpuFailFn(GenState *gen) {
    LLVMValueRef fn = LLVMGetNamedFunction(gen->module, "cone.gpu.fail");
    if (fn)
        return fn;
    LLVMTypeRef parms[4] = {genlI32(gen), LLVMPointerTypeInContext(gen->context, 0), genlI32(gen), genlI32(gen)};
    fn = LLVMAddFunction(gen->module, "cone.gpu.fail",
        LLVMFunctionType(LLVMVoidTypeInContext(gen->context), parms, 4, 0));
    genlFnAttr(gen, fn, "noreturn");
    genlFnAttr(gen, fn, "cold");
    genlFnAttr(gen, fn, "nounwind");
    return fn;
}

// A value as the 32-bit word a record holds
static LLVMValueRef genlGpuWord(GenState *gen, LLVMValueRef val) {
    LLVMTypeRef type = LLVMTypeOf(val);
    if (LLVMGetTypeKind(type) != LLVMIntegerTypeKind)
        return genlI32Const(gen, 0);
    unsigned width = LLVMGetIntTypeWidth(type);
    if (width == 32)
        return val;
    return width > 32 ? LLVMBuildTrunc(gen->builder, val, genlI32(gen), "")
        : LLVMBuildZExt(gen->builder, val, genlI32(gen), "");
}

// Where a check the compiler inserted has failed, on a GPU target: the call
// a kernel records (genlPanic's GPU form). Leaves the block for the caller
// to terminate.
void genlGpuFailCheck(GenState *gen, INode *site, int kind, LLVMValueRef value) {
    size_t filelen;
    char *file = genlSrcFileName(site, &filelen);
    int gpukind = kind == PanicIndex ? GpuFailIndex : kind == PanicSlice ? GpuFailSlice : GpuFailAlloc;
    LLVMValueRef args[4] = {
        genlI32Const(gen, gpukind),
        genlSrcFileText(gen, file, filelen),
        genlI32Const(gen, site->linenbr),
        value ? genlGpuWord(gen, value) : genlI32Const(gen, 0)
    };
    genlGpuCall(gen, genlGpuFailFn(gen), args, 4);
}

// Whether a function is core's 'panic', conestd's 'cone_panic', which 'assert',
// 'unreachable' and 'todo' call too
int genlIsConePanic(FnDclNode *fndcl) {
    return fndcl->dclinfo.cname != NULL && strcmp(fndcl->dclinfo.cname, "cone_panic") == 0
        && (fndcl->dclinfo.facts & DclExternal);
}

// A call to core's 'panic' on a GPU target, given its arguments (the message,
// the file and the line): the failure call a kernel records. The message has
// nowhere to go and is dropped; the file is the slice's address, which is the
// source file's global once the call is inlined into a kernel.
LLVMValueRef genlGpuPanic(GenState *gen, LLVMValueRef *args) {
    LLVMValueRef fargs[4] = {
        genlI32Const(gen, GpuFailPanic),
        LLVMBuildExtractValue(gen->builder, args[1], 0, ""),
        genlGpuWord(gen, args[2]),
        genlI32Const(gen, 0)
    };
    return genlGpuCall(gen, genlGpuFailFn(gen), fargs, 4);
}

// ---- The C library's math ---------------------------------------------------

// No GPU has the C library. A call to one of its math functions, by its C
// symbol (libc's bindings, or any '@c' declaration of the same symbol), is on
// a GPU target the LLVM intrinsic of the same meaning, which LLVM's SPIR-V
// backend selects as the GLSL.std.450 extended instruction ('fmod' is LLVM's
// 'frem', SPIR-V's OpFRem, which also takes its sign from x). The table
// holds each function C names in its double form and its float form, 'f'
// after it.
typedef struct GenlGpuMathFn {
    const char *cname;      // the double form's C name
    const char *intrinsic;  // LLVM's intrinsic, overloaded on the float type; NULL for 'frem'
    unsigned nargs;
} GenlGpuMathFn;

static const GenlGpuMathFn genlGpuMathFns[] = {
    {"sqrt", "llvm.sqrt", 1},
    {"sin", "llvm.sin", 1},
    {"cos", "llvm.cos", 1},
    {"tan", "llvm.tan", 1},
    {"asin", "llvm.asin", 1},
    {"acos", "llvm.acos", 1},
    {"atan", "llvm.atan", 1},
    {"atan2", "llvm.atan2", 2},
    {"exp", "llvm.exp", 1},
    {"log", "llvm.log", 1},
    {"pow", "llvm.pow", 2},
    {"fabs", "llvm.fabs", 1},
    {"floor", "llvm.floor", 1},
    {"ceil", "llvm.ceil", 1},
    {"fmod", NULL, 2},
};

// On a GPU target, a call to a C library math function as its intrinsic,
// given its arguments, or NULL when the function is no such one. 'fabsf',
// which libc writes inline as 'fabs' widened and narrowed (the UCRT has no
// symbol for it), is caught here before its body is expanded, so the module
// asks for no 64-bit float.
LLVMValueRef genlGpuMath(GenState *gen, FnDclNode *fndcl, LLVMValueRef *args, unsigned nargs) {
    if (!(fndcl->dclinfo.facts & DclCName) || nargs == 0 || nargs > 2)
        return NULL;
    char symbol[2048];
    nameSymbol(symbol, (INode*)fndcl);
    size_t len = strlen(symbol);
    LLVMTypeRef ftype = LLVMTypeOf(args[0]);
    LLVMTypeKind want = LLVMDoubleTypeKind;
    if (len > 1 && symbol[len - 1] == 'f') {
        want = LLVMFloatTypeKind;
        --len;
    }
    for (size_t i = 0; i < sizeof(genlGpuMathFns) / sizeof(genlGpuMathFns[0]); ++i) {
        const GenlGpuMathFn *mathfn = &genlGpuMathFns[i];
        if (strlen(mathfn->cname) != len || strncmp(mathfn->cname, symbol, len) != 0 || mathfn->nargs != nargs)
            continue;
        for (unsigned a = 0; a < nargs; ++a) {
            if (LLVMGetTypeKind(LLVMTypeOf(args[a])) != want)
                return NULL;
        }
        if (mathfn->intrinsic == NULL)
            return LLVMBuildFRem(gen->builder, args[0], args[1], "");
        return genlGpuCall(gen, genlGpuIntrinsic(gen, mathfn->intrinsic, &ftype, 1), args, nargs);
    }
    return NULL;
}

// Whether a C symbol taking 'nargs' arguments is one of the C library's math
// functions a GPU lowers (genlGpuMath), in either form. Type checking asks it
// to refuse a call of any other function defined elsewhere: nothing else of
// the C library, nor any other code, is there on a GPU.
int genlGpuIsMathSymbol(const char *symbol, unsigned nargs) {
    size_t len = strlen(symbol);
    if (len > 1 && symbol[len - 1] == 'f')
        --len;
    for (size_t i = 0; i < sizeof(genlGpuMathFns) / sizeof(genlGpuMathFns[0]); ++i) {
        const GenlGpuMathFn *mathfn = &genlGpuMathFns[i];
        if (strlen(mathfn->cname) == len && strncmp(mathfn->cname, symbol, len) == 0 && mathfn->nargs == nargs)
            return 1;
    }
    return 0;
}

// ---- The kernel -------------------------------------------------------------

// The name a binding is given in the module (an OpName): LLVM's SPIR-V
// backend reads it from a constant global and keeps one variable per name,
// so each is unique, '<kernel>.<parameter>'. It is held as 32-bit words, the
// text's bytes four to a word, not as bytes: a byte array would ask the
// module for 8-bit integers, which WebGPU lacks.
static LLVMValueRef genlGpuBindingName(GenState *gen, const char *kernel, const char *parm) {
    char text[512];
    snprintf(text, sizeof(text), "%s.%s", kernel, parm);
    size_t len = strlen(text);
    uint32_t nwords = (uint32_t)(len / 4 + 1);
    LLVMValueRef *words = (LLVMValueRef *)memAllocBlk(nwords * sizeof(LLVMValueRef));
    for (uint32_t w = 0; w < nwords; ++w) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4; ++b) {
            size_t at = w * 4 + b;
            if (at < len)
                word |= (uint32_t)(unsigned char)text[at] << (8 * b);
        }
        words[w] = genlI32Const(gen, word);
    }
    LLVMValueRef init = LLVMConstArray2(genlI32(gen), words, nwords);
    LLVMValueRef global = LLVMAddGlobalInAddressSpace(gen->module, LLVMTypeOf(init), "binding", 0);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetInitializer(global, init);
    return global;
}

// The type of a storage buffer's handle: a run-time array of 'elem', or one
// 'elem' when 'single', read-only or read-write
static LLVMTypeRef genlGpuBufferType(GenState *gen, LLVMTypeRef elem, int single, int writes) {
    LLVMTypeRef contents = single ? elem : LLVMArrayType2(elem, 0);
    unsigned ints[2] = {12, writes ? 1 : 0};     // StorageBuffer's storage class; written
    return LLVMTargetExtTypeInContext(gen->context, "spirv.VulkanBuffer", &contents, 1, ints, 2);
}

// The handle of the storage buffer at descriptor set 0, 'binding'
static LLVMValueRef genlGpuHandle(GenState *gen, LLVMTypeRef handletype, unsigned binding, LLVMValueRef name) {
    LLVMValueRef args[5] = {genlI32Const(gen, 0), genlI32Const(gen, binding), genlI32Const(gen, 1),
        genlI32Const(gen, 0), name};
    return genlGpuCall(gen, genlGpuIntrinsic(gen, "llvm.spv.resource.handlefrombinding", &handletype, 1), args, 5);
}

// The address of element 'index' of a buffer's run-time array
static LLVMValueRef genlGpuElemPtr(GenState *gen, LLVMValueRef handle, LLVMValueRef index) {
    LLVMTypeRef over[3] = {LLVMPointerTypeInContext(gen->context, GenlStorageBuffer), LLVMTypeOf(handle), genlI32(gen)};
    LLVMValueRef args[2] = {handle, index};
    return genlGpuCall(gen, genlGpuIntrinsic(gen, "llvm.spv.resource.getpointer", over, 3), args, 2);
}

// How many elements a buffer's run-time array has: what SPIR-V's OpArrayLength
// answers. LLVM 23's SPIR-V backend selects no instruction for it, so the
// kernel calls a function of this name, declared and never defined, which
// genlGpuPatch turns into the instruction once the module is emitted. One a
// buffer, since each handle is a type of its own.
static LLVMValueRef genlGpuArrayLength(GenState *gen, LLVMValueRef handle, const char *kernel, unsigned binding) {
    char name[300];
    snprintf(name, sizeof(name), "cone.arraylength.%s.%u", kernel, binding);
    LLVMTypeRef handletype = LLVMTypeOf(handle);
    LLVMValueRef fn = LLVMAddFunction(gen->module, name, LLVMFunctionType(genlI32(gen), &handletype, 1, 0));
    genlFnAttr(gen, fn, "nounwind");
    genlFnAttr(gen, fn, "willreturn");
    unsigned memory = LLVMGetEnumAttributeKindForName("memory", 6);
    LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(gen->context, memory, 0));
    return genlGpuCall(gen, fn, &handle, 1);
}

// A value of 'type' loaded from a storage buffer, a scalar at a time: a
// struct read whole from a buffer would be one of the buffer's laid-out type,
// which SPIR-V does not let a local hold
static LLVMValueRef genlGpuLoadLeaves(GenState *gen, LLVMTypeRef roottype, LLVMValueRef base,
        LLVMTypeRef type, LLVMValueRef *idx, unsigned nidx) {
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind) {
        LLVMValueRef ptr = LLVMBuildInBoundsGEP2(gen->builder, roottype, base, idx, nidx, "");
        return LLVMBuildLoad2(gen->builder, type, ptr, "");
    }
    LLVMValueRef agg = LLVMGetUndef(type);
    unsigned cnt = kind == LLVMStructTypeKind ? LLVMCountStructElementTypes(type) : (unsigned)LLVMGetArrayLength2(type);
    for (unsigned i = 0; i < cnt; ++i) {
        LLVMTypeRef elem = kind == LLVMStructTypeKind ? LLVMStructGetTypeAtIndex(type, i) : LLVMGetElementType(type);
        idx[nidx] = genlI32Const(gen, i);
        agg = LLVMBuildInsertValue(gen->builder, agg, genlGpuLoadLeaves(gen, roottype, base, elem, idx, nidx + 1), i, "");
    }
    return agg;
}

// A value of 'type' stored into a storage buffer a scalar at a time, for the
// same reason
static void genlGpuStoreLeaves(GenState *gen, LLVMTypeRef roottype, LLVMValueRef base,
        LLVMValueRef val, LLVMValueRef *idx, unsigned nidx) {
    LLVMTypeRef type = LLVMTypeOf(val);
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind) {
        LLVMBuildStore(gen->builder, val, LLVMBuildInBoundsGEP2(gen->builder, roottype, base, idx, nidx, ""));
        return;
    }
    unsigned cnt = kind == LLVMStructTypeKind ? LLVMCountStructElementTypes(type) : (unsigned)LLVMGetArrayLength2(type);
    for (unsigned i = 0; i < cnt; ++i) {
        idx[nidx] = genlI32Const(gen, i);
        genlGpuStoreLeaves(gen, roottype, base, LLVMBuildExtractValue(gen->builder, val, i, ""), idx, nidx + 1);
    }
}

// Every struct or array a function loads from or stores into a storage buffer
// whole, loaded or stored a scalar at a time: LLVM 23's SPIR-V backend gives
// the buffer's laid-out struct and a local's two types, and a whole one's
// store between them is a module the validator refuses. One of more than
// 256 scalars is left whole.
void genlGpuBufferAccess(GenState *gen, LLVMValueRef fn) {
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        LLVMValueRef inst = LLVMGetFirstInstruction(blk);
        while (inst) {
            LLVMValueRef next = LLVMGetNextInstruction(inst);
            int isload = LLVMIsALoadInst(inst) != NULL;
            int isstore = LLVMIsAStoreInst(inst) != NULL;
            LLVMValueRef ptr = isload ? LLVMGetOperand(inst, 0) : isstore ? LLVMGetOperand(inst, 1) : NULL;
            LLVMTypeRef type = isload ? LLVMTypeOf(inst) : isstore ? LLVMTypeOf(LLVMGetOperand(inst, 0)) : NULL;
            LLVMTypeKind kind = type ? LLVMGetTypeKind(type) : LLVMVoidTypeKind;
            if (ptr && (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind)
                && LLVMGetPointerAddressSpace(LLVMTypeOf(ptr)) == GenlStorageBuffer
                && LLVMSizeOfTypeInBits(gen->datalayout, type) <= 256 * 32) {
                LLVMValueRef idx[64];
                idx[0] = genlI32Const(gen, 0);
                LLVMPositionBuilderBefore(gen->builder, inst);
                if (isload)
                    LLVMReplaceAllUsesWith(inst, genlGpuLoadLeaves(gen, type, ptr, type, idx, 1));
                else
                    genlGpuStoreLeaves(gen, type, ptr, LLVMGetOperand(inst, 0), idx, 1);
                LLVMInstructionEraseFromParent(inst);
            }
            inst = next;
        }
    }
}

// An Invocation filled from the built-ins
static LLVMValueRef genlGpuInvocation(GenState *gen, LLVMTypeRef type) {
    static const char *ids[] = {"llvm.spv.thread.id", "llvm.spv.thread.id.in.group", "llvm.spv.group.id",
        NULL, "llvm.spv.num.workgroups"};
    LLVMValueRef inv = LLVMGetUndef(type);
    for (unsigned field = 0; field < 5; ++field) {
        LLVMValueRef part;
        if (ids[field] == NULL)
            part = genlGpuCall(gen, genlGpuIntrinsic(gen, "llvm.spv.flattened.thread.id.in.group", NULL, 0), NULL, 0);
        else {
            part = LLVMGetUndef(LLVMStructGetTypeAtIndex(type, field));
            for (int dim = 0; dim < 3; ++dim)
                part = LLVMBuildInsertValue(gen->builder, part, genlGpuBuiltin(gen, ids[field], dim), dim, "");
        }
        inv = LLVMBuildInsertValue(gen->builder, inv, part, field, "");
    }
    return inv;
}

// Make the kernel for compute entry point 'fnnode', whose Cone function has
// just been generated
void genlComputeEntry(GenState *gen, FnDclNode *fnnode) {
    if (!gen->opt->gpu)
        return;
    if (!gen->opt->vulkan) {
        errorMsgNode((INode *)fnnode, ErrorComputeTarget,
            "A compute entry point is made for SPIR-V's Vulkan form ('--triple=spirv1.6-unknown-vulkan1.3'); its OpenCL form has kernels of another kind.");
        return;
    }
    char *name = &fnnode->namesym->namestr;
    LLVMValueRef body = fnnode->llvmvar;
    size_t symlen;
    const char *symbol = LLVMGetValueName2(body, &symlen);
    // The kernel takes the name; the Cone function, inlined into it, gives
    // way if its symbol is the same (a root module's names are bare)
    if (strcmp(symbol, name) == 0) {
        char renamed[512];
        snprintf(renamed, sizeof(renamed), "%s.body", name);
        LLVMSetValueName2(body, renamed, strlen(renamed));
    }
    if (LLVMGetNamedFunction(gen->module, name)) {
        errorMsgNode((INode *)fnnode, ErrorComputeAttr,
            "Two compute entry points are named '%s' in this compile, and a dispatch finds a kernel by its name.", name);
        return;
    }
    LLVMValueRef kernel = LLVMAddFunction(gen->module, name,
        LLVMFunctionType(LLVMVoidTypeInContext(gen->context), NULL, 0, 0));
    char sizes[64];
    snprintf(sizes, sizeof(sizes), "%u,%u,%u", fnnode->compute[0], fnnode->compute[1], fnnode->compute[2]);
    LLVMAddAttributeAtIndex(kernel, LLVMAttributeFunctionIndex,
        LLVMCreateStringAttribute(gen->context, "hlsl.shader", 11, "compute", 7));
    LLVMAddAttributeAtIndex(kernel, LLVMAttributeFunctionIndex,
        LLVMCreateStringAttribute(gen->context, "hlsl.numthreads", 15, sizes, (unsigned)strlen(sizes)));

    LLVMBuilderRef svbuilder = gen->builder;
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(gen->builder, LLVMAppendBasicBlockInContext(gen->context, kernel, "entry"));
    // A debug build gives the kernel a subprogram too, so that what is
    // inlined into it keeps its lines
    if (!gen->opt->release) {
        LLVMMetadataRef difile = genlDiFile(gen, (INode*)fnnode);
        LLVMMetadataRef fntype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder, difile, NULL, 0, 0);
        LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, difile,
            name, strlen(name), name, strlen(name), difile, fnnode->linenbr, fntype, 0, 1,
            fnnode->linenbr, LLVMDIFlagPublic, 0);
        LLVMSetSubprogram(kernel, sp);
        LLVMSetCurrentDebugLocation2(gen->builder,
            LLVMDIBuilderCreateDebugLocation(gen->context, fnnode->linenbr, 0, sp, NULL));
    }

    // Each parameter, as the dispatch binds it
    FnSigNode *sig = (FnSigNode *)fnnode->vtype;
    LLVMTypeRef bodytype = LLVMGlobalGetValueType(body);
    unsigned nparms = LLVMCountParamTypes(bodytype);
    LLVMTypeRef *parmtypes = (LLVMTypeRef *)memAllocBlk((nparms ? nparms : 1) * sizeof(LLVMTypeRef));
    LLVMGetParamTypes(bodytype, parmtypes);
    assert(nparms == sig->parms->used && "an entry point's parameters each cross as one value");
    LLVMValueRef *args = (LLVMValueRef *)memAllocBlk((nparms ? nparms : 1) * sizeof(LLVMValueRef));
    unsigned binding = 0;
    INode **nodesp;
    uint32_t cnt;
    unsigned i = 0;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode *)*nodesp;
        LLVMTypeRef ptype = parmtypes[i];
        INode *dcl = itypeGetTypeDcl(parm->vtype);
        if (invocationIsCore(parm->vtype))
            args[i] = genlGpuInvocation(gen, ptype);
        else if (dcl->tag == ArrayRefTag) {
            int writes = (permGetFlags(((RefNode *)dcl)->perm) & MayWrite) != 0;
            LLVMTypeRef elem = genlType(gen, ((RefNode *)dcl)->vtexp);
            LLVMValueRef handle = genlGpuHandle(gen, genlGpuBufferType(gen, elem, 0, writes), binding,
                genlGpuBindingName(gen, name, &parm->namesym->namestr));
            LLVMValueRef first = LLVMBuildAddrSpaceCast(gen->builder, genlGpuElemPtr(gen, handle, genlI32Const(gen, 0)),
                LLVMPointerTypeInContext(gen->context, 0), "");
            LLVMValueRef slice = LLVMGetUndef(ptype);
            slice = LLVMBuildInsertValue(gen->builder, slice, first, 0, "");
            args[i] = LLVMBuildInsertValue(gen->builder, slice, genlGpuArrayLength(gen, handle, name, binding), 1, "");
            ++binding;
        }
        else {
            // A struct by value: a buffer holding one
            LLVMValueRef handle = genlGpuHandle(gen, genlGpuBufferType(gen, ptype, 1, 0), binding,
                genlGpuBindingName(gen, name, &parm->namesym->namestr));
            LLVMTypeRef over[2] = {LLVMPointerTypeInContext(gen->context, GenlStorageBuffer), LLVMTypeOf(handle)};
            LLVMValueRef base = genlGpuCall(gen, genlGpuIntrinsic(gen, "llvm.spv.resource.getbasepointer", over, 2), &handle, 1);
            LLVMValueRef idx[64];
            idx[0] = genlI32Const(gen, 0);
            args[i] = genlGpuLoadLeaves(gen, ptype, base, ptype, idx, 1);
            ++binding;
        }
        ++i;
    }
    LLVMBuildCall2(gen->builder, bodytype, body, args, nparms, "");
    LLVMBuildRetVoid(gen->builder);
    LLVMDisposeBuilder(gen->builder);
    gen->builder = svbuilder;

    // Remembered for after the GPU pipeline: the kernel, the binding its
    // error buffer takes, and its declaration
    if (gen->entrycnt == gen->entrymax) {
        gen->entrymax = gen->entrymax ? gen->entrymax * 2 : 8;
        gen->entries = (GenlEntry *)realloc(gen->entries, gen->entrymax * sizeof(GenlEntry));
    }
    gen->entries[gen->entrycnt].kernel = kernel;
    gen->entries[gen->entrycnt].errbinding = binding;
    gen->entries[gen->entrycnt].fndcl = fnnode;
    ++gen->entrycnt;
}

// ---- Settled in the kernel, once everything is inlined ----------------------

// The id of the source file whose name 'ptr' points at, as an i32: a
// constant for one file's global, a choice of constants where the optimizer
// merged failures in several files, else 0
static LLVMValueRef genlGpuFileId(GenState *gen, LLVMValueRef ptr, int depth) {
    while (ptr && LLVMIsAConstantExpr(ptr) && (LLVMGetConstOpcode(ptr) == LLVMAddrSpaceCast
        || LLVMGetConstOpcode(ptr) == LLVMBitCast || LLVMGetConstOpcode(ptr) == LLVMGetElementPtr))
        ptr = LLVMGetOperand(ptr, 0);
    while (ptr && (LLVMIsAAddrSpaceCastInst(ptr) || LLVMIsABitCastInst(ptr)))
        ptr = LLVMGetOperand(ptr, 0);
    if (ptr == NULL)
        return genlI32Const(gen, 0);
    if (LLVMIsAGlobalVariable(ptr))
        return genlI32Const(gen, genlSrcFileId(gen, ptr));
    if (depth < 4 && LLVMIsASelectInst(ptr)) {
        LLVMValueRef a = genlGpuFileId(gen, LLVMGetOperand(ptr, 1), depth + 1);
        LLVMValueRef b = genlGpuFileId(gen, LLVMGetOperand(ptr, 2), depth + 1);
        return a == b ? a : LLVMBuildSelect(gen->builder, LLVMGetOperand(ptr, 0), a, b, "");
    }
    if (depth < 4 && LLVMIsAPHINode(ptr)) {
        unsigned nin = LLVMCountIncoming(ptr);
        LLVMValueRef *ids = (LLVMValueRef *)memAllocBlk(nin * sizeof(LLVMValueRef));
        int same = 1;
        for (unsigned in = 0; in < nin; ++in) {
            LLVMValueRef from = LLVMGetIncomingValue(ptr, in);
            ids[in] = LLVMIsAPHINode(from) || LLVMIsASelectInst(from) ? genlI32Const(gen, 0) : genlGpuFileId(gen, from, depth + 1);
            same = same && ids[in] == ids[0];
        }
        if (same)
            return ids[0];
        // Beside the phi it stands for; the caller positions the builder again
        LLVMPositionBuilderBefore(gen->builder, ptr);
        LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlI32(gen), "");
        for (unsigned in = 0; in < nin; ++in) {
            LLVMBasicBlockRef inblk = LLVMGetIncomingBlock(ptr, in);
            LLVMAddIncoming(phi, &ids[in], &inblk, 1);
        }
        return phi;
    }
    return genlI32Const(gen, 0);
}

// Each failure call in a kernel becomes the record of the failure and the
// kernel's return: the count of failures goes up by one, atomically, and the
// invocation that made it 1 writes what failed, where and which invocation it
// was; then the invocation stops, leaving the kernel, which everything else
// is inlined into. This is done in every build: no check is dropped.
static void genlGpuRecord(GenState *gen, GenlEntry *entry) {
    LLVMValueRef failfn = LLVMGetNamedFunction(gen->module, "cone.gpu.fail");
    if (failfn == NULL)
        return;
    uint32_t ncalls = 0;
    for (LLVMUseRef use = LLVMGetFirstUse(failfn); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMIsACallInst(user) && LLVMGetBasicBlockParent(LLVMGetInstructionParent(user)) == entry->kernel)
            ++ncalls;
    }
    if (ncalls == 0)
        return;
    LLVMValueRef *calls = (LLVMValueRef *)memAllocBlk(ncalls * sizeof(LLVMValueRef));
    ncalls = 0;
    for (LLVMUseRef use = LLVMGetFirstUse(failfn); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMIsACallInst(user) && LLVMGetBasicBlockParent(LLVMGetInstructionParent(user)) == entry->kernel)
            calls[ncalls++] = user;
    }

    // The error buffer, its handle made first in the kernel
    LLVMBasicBlockRef entryblk = LLVMGetEntryBasicBlock(entry->kernel);
    LLVMValueRef first = LLVMGetFirstInstruction(entryblk);
    while (first && LLVMIsAAllocaInst(first))
        first = LLVMGetNextInstruction(first);
    LLVMPositionBuilderBefore(gen->builder, first);
    LLVMSetCurrentDebugLocation2(gen->builder, NULL);
    char *name = &entry->fndcl->namesym->namestr;
    LLVMValueRef handle = genlGpuHandle(gen, genlGpuBufferType(gen, genlI32(gen), 0, 1), entry->errbinding,
        genlGpuBindingName(gen, name, "errors"));
    unsigned device = LLVMGetSyncScopeID(gen->context, "device", 6);

    for (uint32_t c = 0; c < ncalls; ++c) {
        LLVMValueRef call = calls[c];
        LLVMBasicBlockRef blk = LLVMGetInstructionParent(call);
        LLVMPositionBuilderBefore(gen->builder, call);
        LLVMValueRef words[FailWords];
        words[FailKind] = LLVMGetOperand(call, 0);
        words[FailFile] = genlGpuFileId(gen, LLVMGetOperand(call, 1), 0);
        LLVMPositionBuilderBefore(gen->builder, call);
        words[FailLine] = LLVMGetOperand(call, 2);
        words[FailValue] = LLVMGetOperand(call, 3);
        LLVMValueRef count = genlGpuElemPtr(gen, handle, genlI32Const(gen, FailCount));
        LLVMValueRef before = LLVMBuildAtomicRMWSyncScope(gen->builder, LLVMAtomicRMWBinOpAdd, count,
            genlI32Const(gen, 1), LLVMAtomicOrderingAcquireRelease, device);
        LLVMValueRef firstfail = LLVMBuildICmp(gen->builder, LLVMIntEQ, before, genlI32Const(gen, 0), "");

        // What followed the call, 'unreachable', goes with it
        LLVMValueRef after = LLVMGetNextInstruction(call);
        while (after) {
            LLVMValueRef next = LLVMGetNextInstruction(after);
            LLVMInstructionEraseFromParent(after);
            after = next;
        }
        LLVMInstructionEraseFromParent(call);

        LLVMBasicBlockRef recblk = LLVMAppendBasicBlockInContext(gen->context, entry->kernel, "failrecord");
        LLVMBasicBlockRef endblk = LLVMAppendBasicBlockInContext(gen->context, entry->kernel, "failreturn");
        LLVMPositionBuilderAtEnd(gen->builder, blk);
        LLVMBuildCondBr(gen->builder, firstfail, recblk, endblk);
        LLVMPositionBuilderAtEnd(gen->builder, recblk);
        for (int dim = 0; dim < 3; ++dim)
            words[FailIdX + dim] = genlGpuBuiltin(gen, "llvm.spv.thread.id", dim);
        for (int w = FailKind; w < FailWords; ++w)
            LLVMBuildStore(gen->builder, words[w], genlGpuElemPtr(gen, handle, genlI32Const(gen, w)));
        LLVMBuildBr(gen->builder, endblk);
        LLVMPositionBuilderAtEnd(gen->builder, endblk);
        LLVMBuildRetVoid(gen->builder);
    }
}

// Whether an address computation is arithmetic on its pointer: its first
// index is not zero, so it steps off the value the pointer points at
static int genlGpuIsArith(LLVMValueRef gep) {
    LLVMValueRef first = LLVMGetOperand(gep, 1);
    return !(LLVMIsAConstantInt(first) && LLVMConstIntGetZExtValue(first) == 0);
}

// 'a + b', as i32s
static LLVMValueRef genlGpuAddIndex(GenState *gen, LLVMValueRef a, LLVMValueRef b) {
    a = genlGpuWord(gen, a);
    b = genlGpuWord(gen, b);
    if (LLVMIsAConstantInt(a) && LLVMConstIntGetZExtValue(a) == 0)
        return b;
    if (LLVMIsAConstantInt(b) && LLVMConstIntGetZExtValue(b) == 0)
        return a;
    return LLVMBuildAdd(gen->builder, a, b, "");
}

// The source node an address computation was made for, recorded as it was
// generated (genlGpuSite), or NULL
INode *genlGpuSiteOf(GenState *gen, LLVMValueRef inst) {
    unsigned kind = LLVMGetMDKindIDInContext(gen->context, "cone.site", 9);
    LLVMValueRef md = LLVMGetMetadata(inst, kind);
    if (md == NULL || gen->gpusites == NULL)
        return NULL;
    LLVMValueRef idx = LLVMGetOperand(md, 0);
    if (idx == NULL || !LLVMIsAConstantInt(idx))
        return NULL;
    uint64_t n = LLVMConstIntGetZExtValue(idx);
    return n < gen->gpusites->used ? nodesGet(gen->gpusites, (uint32_t)n) : NULL;
}

// Mark an address computation on a GPU target with the node it was made for,
// so that a slice that cannot be traced is refused where it is indexed
void genlGpuSite(GenState *gen, LLVMValueRef inst, INode *site) {
    if (!gen->opt->vulkan || !LLVMIsAInstruction(inst))
        return;
    if (gen->gpusites == NULL)
        gen->gpusites = newNodes(16);
    LLVMValueRef idx = genlI32Const(gen, gen->gpusites->used);
    nodesAdd(&gen->gpusites, site);
    unsigned kind = LLVMGetMDKindIDInContext(gen->context, "cone.site", 9);
    LLVMSetMetadata(inst, kind, LLVMMDNodeInContext(gen->context, &idx, 1));
}

static int genlGpuFold(GenState *gen, GenlEntry *entry, LLVMValueRef gep);

// The value the part 'idx' of aggregate 'agg' was put in as, looked for
// through the insertions and extractions that carried it (a struct holding
// slices, built and taken apart once inlined), or NULL
static LLVMValueRef genlGpuPartOf(LLVMValueRef agg, const unsigned *idx, unsigned nidx, int depth) {
    if (depth > 256 || agg == NULL)
        return NULL;
    if (nidx == 0)
        return agg;
    if (LLVMIsAInsertValueInst(agg)) {
        unsigned nins = LLVMGetNumIndices(agg);
        const unsigned *ins = LLVMGetIndices(agg);
        unsigned common = 0;
        while (common < nins && common < nidx && ins[common] == idx[common])
            ++common;
        if (common == nins)         // the part, or inside it
            return genlGpuPartOf(LLVMGetOperand(agg, 1), idx + nins, nidx - nins, depth + 1);
        if (common == nidx)         // holds the insertion: not one value
            return NULL;
        return genlGpuPartOf(LLVMGetOperand(agg, 0), idx, nidx, depth + 1);
    }
    if (LLVMIsAExtractValueInst(agg)) {
        unsigned next = LLVMGetNumIndices(agg);
        if (next + nidx > 64)
            return NULL;
        unsigned all[64];
        memcpy(all, LLVMGetIndices(agg), next * sizeof(unsigned));
        memcpy(all + next, idx, nidx * sizeof(unsigned));
        return genlGpuPartOf(LLVMGetOperand(agg, 0), all, next + nidx, depth + 1);
    }
    if (LLVMIsAConstant(agg)) {
        LLVMValueRef part = LLVMGetAggregateElement(agg, idx[0]);
        return part ? genlGpuPartOf(part, idx + 1, nidx - 1, depth + 1) : NULL;
    }
    return NULL;
}

// An address computation's replacement: 'base' indexed by 'idx' and then the
// rest of 'gep''s indices, cast to the space 'gep' was in
static void genlGpuReplace(GenState *gen, LLVMValueRef gep, LLVMValueRef newgep) {
    if (LLVMTypeOf(newgep) != LLVMTypeOf(gep))
        newgep = LLVMBuildAddrSpaceCast(gen->builder, newgep, LLVMTypeOf(gep), "");
    LLVMReplaceAllUsesWith(gep, newgep);
    LLVMInstructionEraseFromParent(gep);
}

// SPIR-V's logical addressing has no pointer arithmetic: an address is an
// access chain from a variable. A slice is a pointer and a count, and indexing
// it, or taking a slice of part of it, is arithmetic on its pointer; so once
// everything is inlined into the kernel, each such step is folded into the
// access chain of what the slice came from: a buffer's run-time array (the
// index added to the buffer element's), a fixed array (the index added to the
// element's, or the array indexed when the slice is all of it). What came
// from neither cannot be indexed on a GPU, and is refused here, where the
// arithmetic is: a pointer chosen at run time is, before this, refused by the
// loan walk. Returns 0 once it has refused.
static int genlGpuFold(GenState *gen, GenlEntry *entry, LLVMValueRef gep) {
    LLVMValueRef base = LLVMGetOperand(gep, 0);
    // A pointer taken out of a struct that was built here is the pointer
    // put in it
    if (LLVMIsAExtractValueInst(base)) {
        LLVMValueRef part = genlGpuPartOf(LLVMGetOperand(base, 0), LLVMGetIndices(base), LLVMGetNumIndices(base), 0);
        if (part && part != base && LLVMTypeOf(part) == LLVMTypeOf(base)) {
            LLVMSetOperand(gep, 0, part);
            base = part;
        }
    }
    LLVMValueRef origin = base;
    while (LLVMIsAAddrSpaceCastInst(origin) || (LLVMIsAConstantExpr(origin) && LLVMGetConstOpcode(origin) == LLVMAddrSpaceCast))
        origin = LLVMGetOperand(origin, 0);
    if (LLVMIsAGetElementPtrInst(origin) && genlGpuIsArith(origin)) {
        if (!genlGpuFold(gen, entry, origin))
            return 0;
        return genlGpuFold(gen, entry, gep);
    }

    LLVMTypeRef elemtype = LLVMGetGEPSourceElementType(gep);
    int nops = LLVMGetNumOperands(gep);
    LLVMValueRef step = LLVMGetOperand(gep, 1);
    LLVMValueRef idx[64];
    LLVMPositionBuilderBefore(gen->builder, gep);

    // What the origin points to: a buffer's element, or what an alloca, a
    // global or an address computation is known to
    LLVMValueRef callee = LLVMIsACallInst(origin) ? LLVMGetCalledValue(origin) : NULL;
    LLVMValueRef handle = NULL;
    LLVMTypeRef pointee = NULL;
    if (callee && LLVMIsAFunction(callee)
        && LLVMGetIntrinsicID(callee) == LLVMLookupIntrinsicID("llvm.spv.resource.getpointer", 28)) {
        handle = LLVMGetOperand(origin, 0);
        LLVMTypeRef contents = LLVMGetTargetExtTypeTypeParam(LLVMTypeOf(handle), 0);
        if (LLVMGetTypeKind(contents) == LLVMArrayTypeKind)
            pointee = LLVMGetElementType(contents);
    }
    else
        pointee = genlGpuPointee(origin);

    // A buffer's element: the element 'step' on from it
    if (nops < 60 && handle && pointee == elemtype) {
        LLVMValueRef elem = genlGpuElemPtr(gen, handle, genlGpuAddIndex(gen, LLVMGetOperand(origin, 1), step));
        if (nops > 2) {
            int n = 0;
            idx[n++] = genlI32Const(gen, 0);
            for (int op = 2; op < nops; ++op)
                idx[n++] = LLVMGetOperand(gep, op);
            elem = LLVMBuildInBoundsGEP2(gen->builder, elemtype, elem, idx, n, "");
        }
        genlGpuReplace(gen, gep, elem);
        return 1;
    }

    // All of a fixed array, which the origin is, or holds as its first field
    // (as deep as it goes: optimization drops an address computation whose
    // indices are all zero): the array, indexed
    int depth = 0;
    LLVMTypeRef array = pointee;
    while (array && depth < 16 && !(LLVMGetTypeKind(array) == LLVMArrayTypeKind && LLVMGetElementType(array) == elemtype)) {
        LLVMTypeKind kind = LLVMGetTypeKind(array);
        array = kind == LLVMStructTypeKind && LLVMCountStructElementTypes(array) > 0 ? LLVMStructGetTypeAtIndex(array, 0)
            : kind == LLVMArrayTypeKind ? LLVMGetElementType(array) : NULL;
        ++depth;
    }
    if (array && depth < 16 && nops + depth < 60) {
        int n = 0;
        idx[n++] = genlI32Const(gen, 0);
        for (int d = 0; d < depth; ++d)
            idx[n++] = genlI32Const(gen, 0);
        idx[n++] = step;
        for (int op = 2; op < nops; ++op)
            idx[n++] = LLVMGetOperand(gep, op);
        genlGpuReplace(gen, gep, LLVMBuildInBoundsGEP2(gen->builder, pointee, base, idx, n, ""));
        return 1;
    }

    // An element of a fixed array: the element 'step' on from it
    if (LLVMIsAGetElementPtrInst(origin)) {
        int nbase = LLVMGetNumOperands(origin);
        LLVMTypeRef type = LLVMGetGEPSourceElementType(origin);
        for (int op = 2; op < nbase - 1 && type; ++op)
            type = LLVMGetTypeKind(type) == LLVMStructTypeKind
                ? LLVMStructGetTypeAtIndex(type, (unsigned)LLVMConstIntGetZExtValue(LLVMGetOperand(origin, op)))
                : LLVMGetElementType(type);
        if (nbase > 2 && nbase + nops < 64 && type && LLVMGetTypeKind(type) == LLVMArrayTypeKind
            && LLVMGetElementType(type) == elemtype) {
            int n = 0;
            for (int op = 1; op < nbase - 1; ++op)
                idx[n++] = LLVMGetOperand(origin, op);
            idx[n++] = genlGpuAddIndex(gen, LLVMGetOperand(origin, nbase - 1), step);
            for (int op = 2; op < nops; ++op)
                idx[n++] = LLVMGetOperand(gep, op);
            LLVMValueRef newgep = LLVMBuildInBoundsGEP2(gen->builder, LLVMGetGEPSourceElementType(origin),
                LLVMGetOperand(origin, 0), idx, n, "");
            genlGpuReplace(gen, gep, newgep);
            return 1;
        }
    }

    INode *site = genlGpuSiteOf(gen, gep);
    errorMsgNode(site ? site : (INode *)entry->fndcl, ErrorGpuSliceOrigin,
        "In kernel '%s', an element is reached by stepping from an address that is neither a buffer parameter's nor a fixed array's: a GPU has no pointer arithmetic, so a slice indexed there must come from one of the two.",
        &entry->fndcl->namesym->namestr);
    return 0;
}

// Each step a kernel takes by arithmetic, folded, until none is left: what a
// fold makes never steps, and folding one may fold the step it starts from
// too, so the kernel is searched again after each
static void genlGpuSlices(GenState *gen, GenlEntry *entry) {
    LLVMValueRef found;
    do {
        found = NULL;
        for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(entry->kernel); blk && !found; blk = LLVMGetNextBasicBlock(blk)) {
            for (LLVMValueRef inst = LLVMGetFirstInstruction(blk); inst && !found; inst = LLVMGetNextInstruction(inst)) {
                if (LLVMIsAGetElementPtrInst(inst) && genlGpuIsArith(inst))
                    found = inst;
            }
        }
    } while (found && genlGpuFold(gen, entry, found));
}

// A kernel's returns made one: each a branch to a block that returns. The
// structurizer (structurizecfg, which runs next) makes structured control
// flow of a function's one exit; each failed check's return made another.
static void genlGpuOneReturn(GenState *gen, GenlEntry *entry) {
    uint32_t nrets = 0;
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(entry->kernel); blk; blk = LLVMGetNextBasicBlock(blk))
        nrets += LLVMIsAReturnInst(LLVMGetBasicBlockTerminator(blk)) != NULL;
    if (nrets < 2)
        return;
    LLVMBasicBlockRef exit = LLVMAppendBasicBlockInContext(gen->context, entry->kernel, "kernelexit");
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(entry->kernel); blk; blk = LLVMGetNextBasicBlock(blk)) {
        LLVMValueRef term = LLVMGetBasicBlockTerminator(blk);
        if (blk == exit || !LLVMIsAReturnInst(term))
            continue;
        LLVMInstructionEraseFromParent(term);
        LLVMPositionBuilderAtEnd(gen->builder, blk);
        LLVMBuildBr(gen->builder, exit);
    }
    LLVMPositionBuilderAtEnd(gen->builder, exit);
    LLVMBuildRetVoid(gen->builder);
}

// Settle each kernel, once the GPU pipeline has inlined everything into it
void genlGpuEntries(GenState *gen) {
    for (uint32_t e = 0; e < gen->entrycnt; ++e) {
        genlGpuRecord(gen, &gen->entries[e]);
        genlGpuOneReturn(gen, &gen->entries[e]);
        genlGpuSlices(gen, &gen->entries[e]);
    }
}

// A signed remainder a - (a sdiv b) * b, in place of srem, which the backend
// emits as OpSRem. The RTX 4060's driver computes OpSRem and OpSMod unsigned
// (-7 % 3 is 0, the remainder of 4294967289), whatever the signedness of the
// type, and OpSDiv correctly. Done after the pipeline: instcombine would fold
// the expansion straight back into an srem.
void genlGpuSignedRem(GenState *gen) {
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
        for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
            LLVMValueRef inst = LLVMGetFirstInstruction(blk);
            while (inst) {
                LLVMValueRef next = LLVMGetNextInstruction(inst);
                if (LLVMGetInstructionOpcode(inst) == LLVMSRem) {
                    LLVMValueRef a = LLVMGetOperand(inst, 0);
                    LLVMValueRef b = LLVMGetOperand(inst, 1);
                    LLVMPositionBuilderBefore(gen->builder, inst);
                    LLVMValueRef quotient = LLVMBuildSDiv(gen->builder, a, b, "");
                    LLVMValueRef product = LLVMBuildMul(gen->builder, quotient, b, "");
                    LLVMValueRef rem = LLVMBuildSub(gen->builder, a, product, "");
                    LLVMReplaceAllUsesWith(inst, rem);
                    LLVMInstructionEraseFromParent(inst);
                }
                inst = next;
            }
        }
    }
}

// ---- The module, once emitted -----------------------------------------------

// SPIR-V's opcodes this reads or writes
enum {
    SpvOpSourceExtension = 4, SpvOpName = 5, SpvOpString = 7, SpvOpSource = 3, SpvOpSourceContinued = 2,
    SpvOpCapability = 17, SpvOpFunction = 54, SpvOpFunctionEnd = 56, SpvOpFunctionCall = 57,
    SpvOpArrayLength = 68, SpvOpDecorate = 71, SpvCapabilityLinkage = 5, SpvDecorationLinkageAttributes = 41
};

// The text a literal string of words holds
static int genlSpvStrIs(uint32_t *words, uint32_t nwords, const char *prefix) {
    size_t len = strlen(prefix);
    for (size_t i = 0; i < len; ++i) {
        uint32_t w = (uint32_t)(i / 4);
        if (w >= nwords || (char)((words[w] >> (8 * (i % 4))) & 0xFF) != prefix[i])
            return 0;
    }
    return 1;
}

// What LLVM's SPIR-V backend cannot say, said in the module it emitted: each
// call to a 'cone.arraylength' function (genlGpuArrayLength) becomes
// OpArrayLength of the buffer it was handed, member 0, and the declarations
// go, with the Linkage capability when nothing else is imported or exported;
// and a kernel's module lists its source files, '"cone.file <id> <name>"'
// each an OpSourceExtension, so that an error buffer's file id can be named.
// 'words' is reallocated; answers its new count.
static size_t genlGpuPatch(GenState *gen, uint32_t **wordsp, size_t nwords) {
    uint32_t *words = *wordsp;
    if (nwords < 5 || gen->entrycnt == 0)
        return nwords;

    // The length functions' ids, and whether any other linkage is left
    uint32_t navail = 16, nlen = 0, others = 0;
    uint32_t *lenids = (uint32_t *)malloc(navail * sizeof(uint32_t));
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords)
            break;
        if (op == SpvOpDecorate && count >= 4 && words[i + 2] == SpvDecorationLinkageAttributes) {
            if (genlSpvStrIs(&words[i + 3], count - 3, "cone.arraylength.")) {
                if (nlen == navail)
                    lenids = (uint32_t *)realloc(lenids, (navail *= 2) * sizeof(uint32_t));
                lenids[nlen++] = words[i + 1];
            }
            else
                ++others;
        }
        i += count;
    }

    // The file list, after any OpSource, before the first OpName
    int nfiles = genlSrcFileCount(gen);
    size_t extra = 0;
    for (int f = 1; f <= nfiles; ++f)
        extra += 1 + (strlen(genlSrcFileAt(gen, f)) + 24) / 4;
    uint32_t *out = (uint32_t *)malloc((nwords + extra) * sizeof(uint32_t));
    memcpy(out, words, 5 * sizeof(uint32_t));
    size_t o = 5;
    int filesdone = 0;
    int infn = 0;
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords) {
            memcpy(&out[o], &words[i], (nwords - i) * sizeof(uint32_t));
            o += nwords - i;
            break;
        }
        uint32_t *inst = &words[i];
        i += count;
        if (!filesdone && (op == SpvOpName || op == 6 || op == SpvOpDecorate || op == 330 || op >= 19)
            && op != SpvOpSourceExtension && op != SpvOpSource && op != SpvOpString && op != SpvOpSourceContinued) {
            for (int f = 1; f <= nfiles; ++f) {
                char text[600];
                snprintf(text, sizeof(text), "cone.file %d %s", f, genlSrcFileAt(gen, f));
                size_t len = strlen(text);
                uint32_t n = (uint32_t)(len / 4 + 1);
                out[o++] = ((n + 1) << 16) | SpvOpSourceExtension;
                for (uint32_t w = 0; w < n; ++w) {
                    uint32_t word = 0;
                    for (uint32_t b = 0; b < 4; ++b) {
                        size_t at = w * 4 + b;
                        if (at < len)
                            word |= (uint32_t)(unsigned char)text[at] << (8 * b);
                    }
                    out[o++] = word;
                }
            }
            filesdone = 1;
        }
        int islen = 0;
        if ((op == SpvOpName || op == SpvOpDecorate) && count >= 2)
            for (uint32_t l = 0; l < nlen; ++l)
                islen = islen || inst[1] == lenids[l];
        if (op == SpvOpFunction && count >= 5)
            for (uint32_t l = 0; l < nlen; ++l)
                infn = infn || inst[2] == lenids[l];
        if (infn) {
            if (op == SpvOpFunctionEnd)
                infn = 0;
            continue;
        }
        if (islen)
            continue;
        if (op == SpvOpCapability && count == 2 && inst[1] == SpvCapabilityLinkage && others == 0 && nlen > 0)
            continue;
        if (op == SpvOpFunctionCall && count == 5) {
            int call = 0;
            for (uint32_t l = 0; l < nlen; ++l)
                call = call || inst[3] == lenids[l];
            if (call) {
                out[o++] = (5u << 16) | SpvOpArrayLength;
                out[o++] = inst[1];
                out[o++] = inst[2];
                out[o++] = inst[4];
                out[o++] = 0;
                continue;
            }
        }
        memcpy(&out[o], inst, count * sizeof(uint32_t));
        o += count;
    }
    free(lenids);
    free(words);
    *wordsp = out;
    return o;
}

// ---- No contraction ---------------------------------------------------------

// A SPIR-V instruction, by opcode and by the name its assembly gives it
typedef struct {
    uint32_t op;
    const char *name;
} GenlSpvOp;

// The float arithmetic decorated NoContraction: every arithmetic instruction
// on floats SPIR-V has, but OpFRem and OpFMod. A driver may fuse a multiply
// and an add into one rounding (a multiply-add), and reassociate, unless the
// result says it may not; the CPU does neither, so without the decoration
// even adds and multiplies differ from the CPU in the last bits. A remainder
// is left free: the CPU's is exact, as C's fmod always is, and the RTX
// 4060's matches it bit for bit only when the driver may fuse inside it.
static const GenlSpvOp genlSpvNoContract[] = {
    {127, "OpFNegate"}, {129, "OpFAdd"}, {131, "OpFSub"}, {133, "OpFMul"}, {136, "OpFDiv"},
    {142, "OpVectorTimesScalar"}, {143, "OpMatrixTimesScalar"}, {144, "OpVectorTimesMatrix"},
    {145, "OpMatrixTimesVector"}, {146, "OpMatrixTimesMatrix"}, {147, "OpOuterProduct"}, {148, "OpDot"}
};

// What a module holds before its types: capabilities, extensions, imports,
// the memory model, entry points and their modes, debug names and strings,
// and last the annotations, after which the new decorations go
static const GenlSpvOp genlSpvPreamble[] = {
    {17, "OpCapability"}, {10, "OpExtension"}, {11, "OpExtInstImport"}, {14, "OpMemoryModel"},
    {15, "OpEntryPoint"}, {16, "OpExecutionMode"}, {331, "OpExecutionModeId"},
    {7, "OpString"}, {4, "OpSourceExtension"}, {3, "OpSource"}, {2, "OpSourceContinued"},
    {5, "OpName"}, {6, "OpMemberName"}, {330, "OpModuleProcessed"},
    {71, "OpDecorate"}, {72, "OpMemberDecorate"}, {73, "OpDecorationGroup"}, {74, "OpGroupDecorate"},
    {75, "OpGroupMemberDecorate"}, {332, "OpDecorateId"}, {5632, "OpDecorateString"},
    {5633, "OpMemberDecorateString"}
};

#define SpvDecorationNoContraction 42
#define genlSpvCnt(a) (sizeof(a) / sizeof((a)[0]))

static int genlSpvOpIn(const GenlSpvOp *ops, size_t n, uint32_t op) {
    for (size_t i = 0; i < n; ++i)
        if (ops[i].op == op)
            return 1;
    return 0;
}

static int genlSpvNameIn(const GenlSpvOp *ops, size_t n, const char *name, size_t len) {
    for (size_t i = 0; i < n; ++i)
        if (strlen(ops[i].name) == len && strncmp(ops[i].name, name, len) == 0)
            return 1;
    return 0;
}

// Decorate the result of every float arithmetic instruction NoContraction
// (genlSpvNoContract), so that no driver fuses or reassociates it, as
// slangc's '-fp-mode precise' does. LLVM 23's SPIR-V backend has no route
// from IR to the decoration, so it is added to the module it emitted, each
// 'OpDecorate <id> NoContraction' after the module's last annotation.
// 'words' is reallocated; answers its new count.
static size_t genlGpuNoContraction(uint32_t **wordsp, size_t nwords) {
    uint32_t *words = *wordsp;
    size_t at = 0, nops = 0;
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords)
            break;
        if (at == 0 && !genlSpvOpIn(genlSpvPreamble, genlSpvCnt(genlSpvPreamble), op))
            at = i;
        if (count >= 3 && genlSpvOpIn(genlSpvNoContract, genlSpvCnt(genlSpvNoContract), op))
            ++nops;
        i += count;
    }
    if (nops == 0 || at == 0)
        return nwords;

    uint32_t *out = (uint32_t *)malloc((nwords + 3 * nops) * sizeof(uint32_t));
    memcpy(out, words, at * sizeof(uint32_t));
    size_t o = at;
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords)
            break;
        if (count >= 3 && genlSpvOpIn(genlSpvNoContract, genlSpvCnt(genlSpvNoContract), op)) {
            out[o++] = (3u << 16) | SpvOpDecorate;
            out[o++] = words[i + 2];
            out[o++] = SpvDecorationNoContraction;
        }
        i += count;
    }
    memcpy(&out[o], &words[at], (nwords - at) * sizeof(uint32_t));
    o += nwords - at;
    free(words);
    *wordsp = out;
    return o;
}

// The same decorations in the module's assembly, LLVM's text of it, an
// instruction a line ('%12 = OpFAdd %7 %10 %11'), so that '--asm' shows what
// the module holds. Answers the new text, which the caller frees, or NULL
// when there is nothing to decorate.
static char *genlGpuNoContractionAsm(const char *text, size_t len) {
    size_t at = len, dlen = 0, davail = 256;
    char *decs = (char *)malloc(davail);
    for (size_t i = 0; i < len;) {
        size_t end = i;
        while (end < len && text[end] != '\n')
            ++end;
        const char *p = &text[i], *lineend = &text[end];
        while (p < lineend && (*p == ' ' || *p == '\t'))
            ++p;
        const char *id = NULL;
        size_t idlen = 0;
        if (p < lineend && *p == '%') {
            id = p;
            while (p < lineend && *p != ' ')
                ++p;
            idlen = p - id;
            while (p < lineend && (*p == ' ' || *p == '='))
                ++p;
        }
        const char *name = p;
        while (p < lineend && *p != ' ' && *p != '\r')
            ++p;
        size_t namelen = p - name;
        if (namelen > 0 && *name != ';') {
            if (at == len && !genlSpvNameIn(genlSpvPreamble, genlSpvCnt(genlSpvPreamble), name, namelen))
                at = i;
            if (id && genlSpvNameIn(genlSpvNoContract, genlSpvCnt(genlSpvNoContract), name, namelen)) {
                while (dlen + idlen + 32 > davail)
                    decs = (char *)realloc(decs, davail *= 2);
                dlen += sprintf(&decs[dlen], "\tOpDecorate %.*s NoContraction\n", (int)idlen, id);
            }
        }
        i = end < len ? end + 1 : end;
    }
    char *out = NULL;
    if (dlen > 0 && at < len) {
        out = (char *)malloc(len + dlen + 1);
        memcpy(out, text, at);
        memcpy(&out[at], decs, dlen);
        memcpy(&out[at + dlen], &text[at], len - at);
        out[len + dlen] = '\0';
    }
    free(decs);
    return out;
}

// Emit a module for SPIR-V's Vulkan form: assembly, if asked, from a copy
// (genlOut says why), and the module itself through genlGpuPatch; both
// decorated against contraction (genlGpuNoContraction)
void genlGpuOut(GenState *gen, char *objpath, char *asmpath) {
    char *err;
    if (asmpath) {
        // Emitted by a target machine of its own: LLVM's SPIR-V backend keeps
        // what it learns of a module's types and values in the machine's
        // subtarget, keyed by their addresses, and a second module emitted
        // by the same machine, the copy disposed between, now and then met
        // a stale entry at a reused address and failed to select an
        // instruction (measured: 4 compiles in 150)
        LLVMModuleRef asmmod = LLVMCloneModule(gen->module);
        LLVMTargetMachineRef asmmachine = genlCreateMachine(gen->opt);
        LLVMMemoryBufferRef asmbuf;
        if (asmmachine == NULL)
            ;
        else if (LLVMTargetMachineEmitToMemoryBuffer(asmmachine, asmmod, LLVMAssemblyFile, &err, &asmbuf) != 0) {
            errorMsg(ErrorGenErr, "Could not emit asm file: %s", err);
            LLVMDisposeMessage(err);
        }
        else {
            const char *text = LLVMGetBufferStart(asmbuf);
            size_t len = LLVMGetBufferSize(asmbuf);
            char *decorated = genlGpuNoContractionAsm(text, len);
            const char *outtext = decorated ? decorated : text;
            size_t outlen = decorated ? strlen(decorated) : len;
            FILE *file = fopen(asmpath, "wb");
            if (file == NULL || fwrite(outtext, 1, outlen, file) != outlen)
                errorMsg(ErrorGenErr, "Could not write asm file: %s", asmpath);
            if (file)
                fclose(file);
            free(decorated);
            LLVMDisposeMemoryBuffer(asmbuf);
        }
        LLVMDisposeModule(asmmod);
        if (asmmachine)
            LLVMDisposeTargetMachine(asmmachine);
    }
    LLVMMemoryBufferRef buf;
    if (LLVMTargetMachineEmitToMemoryBuffer(gen->machine, gen->module, LLVMObjectFile, &err, &buf) != 0) {
        errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
        LLVMDisposeMessage(err);
        return;
    }
    size_t size = LLVMGetBufferSize(buf);
    size_t nwords = size / 4;
    uint32_t *words = (uint32_t *)malloc((nwords ? nwords : 1) * sizeof(uint32_t));
    memcpy(words, LLVMGetBufferStart(buf), nwords * 4);
    LLVMDisposeMemoryBuffer(buf);
    nwords = genlGpuSyncPatch(&words, nwords);
    nwords = genlGpuPatch(gen, &words, nwords);
    nwords = genlGpuNoContraction(&words, nwords);
    FILE *file = fopen(objpath, "wb");
    if (file == NULL || fwrite(words, 4, nwords, file) != nwords)
        errorMsg(ErrorGenErr, "Could not write obj file: %s", objpath);
    if (file)
        fclose(file);
    free(words);
}
