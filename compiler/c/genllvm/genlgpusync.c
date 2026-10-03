/** What invocations share on a GPU: atomics, workgroup memory and barriers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "genllvm.h"

#include <stdlib.h>
#include <string.h>

// Invocations share memory of two kinds on a GPU: a storage buffer, which
// every invocation of a dispatch reaches, and a '@workgroup' global, one copy
// for each workgroup, which its invocations share (SPIR-V's Workgroup storage
// class, LLVM's address space 3; genlGloVarName). An atomic operation is the
// same LLVM instruction on a GPU as on the CPU (genlAtomicIntrinsic), and is
// settled here once the GPU pipeline has given each pointer its origin's
// address space (genlGpuAtomics):
//
// - its scope is the memory's: a buffer's is the device, a workgroup's the
//   workgroup. One LLVM leaves at the system scope would be SPIR-V's
//   CrossDevice, which Vulkan refuses;
// - its ordering is one Vulkan has: sequential consistency, which Vulkan's
//   memory model does not, is acquire-release, as Vulkan's GLSL and HLSL
//   compilers make it. Relaxed stays relaxed: LLVM 23's SPIR-V backend writes
//   the memory's storage-class bit into a relaxed operation's semantics,
//   which Vulkan refuses (a storage class with no ordering to apply it), so
//   once the module is emitted that operand is made None (genlGpuSyncPatch);
// - a compare-and-swap ('cmpxchg') crashes that backend's pointer-cast
//   legalisation, in every form measured, so it is made the backend's own
//   intrinsic for one, 'llvm.spv.cmpxchg', with its scope and semantics
//   written here; the backend gives that intrinsic's result the type of its
//   i32 declaration where its two insertions want a struct, and
//   genlGpuSyncPatch takes the insertions out;
// - in a kernel, one on any other memory (a local, a private global) is
//   refused, ErrorGpuAtomicPlace: Vulkan allows atomics only on memory that
//   invocations share.
//
// 'workgroupBarrier()' and 'storageBarrier()' are core's intrinsics: on a GPU
// each is OpControlBarrier, every invocation of the workgroup waiting there,
// with the workgroup's memory, or the storage buffers', made visible across
// it (genlGpuBarrier). The CPU runs a kernel one invocation at a time, so
// there each is nothing.

// LLVM's address spaces for SPIR-V's Workgroup and StorageBuffer storage classes
#define GenlWorkgroup 3
#define GenlStorageBufferSpace 11

// SPIR-V's scopes and memory semantics
enum {
    SpvScopeDevice = 1, SpvScopeWorkgroup = 2,
    SpvSemAcquire = 0x2, SpvSemRelease = 0x4, SpvSemAcqRel = 0x8, SpvSemSeqCst = 0x10,
    SpvSemUniformMemory = 0x40, SpvSemWorkgroupMemory = 0x100
};

// Each barrier, as LLVM's SPIR-V intrinsic for it (the ones Clang's HLSL makes
// of GroupMemoryBarrierWithGroupSync and DeviceMemoryBarrierWithGroupSync):
// OpControlBarrier, the workgroup's invocations waiting, acquire-release over
// the workgroup's memory at its scope, or the buffers' at the device's
LLVMValueRef genlGpuBarrier(GenState *gen, int storage) {
    const char *name = storage ? "llvm.spv.device.memory.barrier.with.group.sync"
        : "llvm.spv.group.memory.barrier.with.group.sync";
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef fn = LLVMGetIntrinsicDeclaration(gen->module, id, NULL, 0);
    return LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
}

// The address space a global is in on a GPU target: a '@workgroup' global's
// is the workgroup's, every other the data layout's
unsigned genlGpuGlobalSpace(VarDclNode *glovar) {
    return (glovar->dclinfo.facts & DclWorkgroup) ? GenlWorkgroup : 0;
}

// An ordering as SPIR-V's memory semantics on memory of storage class 'sc':
// relaxed is none at all, as Vulkan asks
static unsigned genlGpuSemantics(LLVMAtomicOrdering order, unsigned sc) {
    switch (order) {
    case LLVMAtomicOrderingAcquire: return SpvSemAcquire | sc;
    case LLVMAtomicOrderingRelease: return SpvSemRelease | sc;
    case LLVMAtomicOrderingAcquireRelease:
    case LLVMAtomicOrderingSequentiallyConsistent: return SpvSemAcqRel | sc;
    default: return 0;
    }
}

// A compare-and-swap as 'llvm.spv.cmpxchg': its pointer, the value expected,
// the value to store, then the scope and the two semantics, answering the
// value seen. Whether it stored is whether that is the value expected.
static void genlGpuCompareSwap(GenState *gen, LLVMValueRef cx, unsigned space) {
    LLVMValueRef ptr = LLVMGetOperand(cx, 0);
    LLVMValueRef expected = LLVMGetOperand(cx, 1);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    if (LLVMTypeOf(expected) != i32)
        return;     // On memory no other invocation reaches, refused already
    unsigned sc = space == GenlWorkgroup ? SpvSemWorkgroupMemory : SpvSemUniformMemory;
    LLVMTypeRef ptrtype = LLVMTypeOf(ptr);
    unsigned id = LLVMLookupIntrinsicID("llvm.spv.cmpxchg", 16);
    LLVMValueRef fn = LLVMGetIntrinsicDeclaration(gen->module, id, &ptrtype, 1);
    LLVMValueRef args[6] = {ptr, expected, LLVMGetOperand(cx, 2),
        LLVMConstInt(i32, space == GenlWorkgroup ? SpvScopeWorkgroup : SpvScopeDevice, 0),
        LLVMConstInt(i32, genlGpuSemantics(LLVMGetCmpXchgSuccessOrdering(cx), sc), 0),
        LLVMConstInt(i32, genlGpuSemantics(LLVMGetCmpXchgFailureOrdering(cx), sc) ? SpvSemAcquire | sc : 0, 0)};
    LLVMPositionBuilderBefore(gen->builder, cx);
    LLVMValueRef seen = LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(fn), fn, args, 6, "");
    LLVMValueRef stored = LLVMBuildICmp(gen->builder, LLVMIntEQ, seen, expected, "");
    // Each part taken out is the part; anything taking the pair whole gets it
    // made again
    LLVMValueRef pair = NULL;
    LLVMUseRef use = LLVMGetFirstUse(cx);
    while (use) {
        LLVMValueRef user = LLVMGetUser(use);
        use = LLVMGetNextUse(use);
        if (LLVMIsAExtractValueInst(user) && LLVMGetNumIndices(user) == 1) {
            LLVMReplaceAllUsesWith(user, LLVMGetIndices(user)[0] == 0 ? seen : stored);
            LLVMInstructionEraseFromParent(user);
        }
        else if (pair == NULL) {
            pair = LLVMBuildInsertValue(gen->builder, LLVMGetUndef(LLVMTypeOf(cx)), seen, 0, "");
            pair = LLVMBuildInsertValue(gen->builder, pair, stored, 1, "");
        }
    }
    if (pair)
        LLVMReplaceAllUsesWith(cx, pair);
    LLVMInstructionEraseFromParent(cx);
}

// Mark an atomic instruction with the call it was made for, so that one on
// memory no other invocation reaches is refused where it is written: a call
// to mem's intrinsic in the program's own code. One in core's Atomic methods
// is not marked, since the program did not write it there; its refusal names
// the kernel instead.
void genlGpuAtomicSite(GenState *gen, LLVMValueRef inst, INode *call) {
    ModuleNode *mod = gen->fndcl ? dclInfoGetModule((INode *)gen->fndcl) : NULL;
    if (mod && mod->dclinfo.owner == NULL && mod->namesym && strcmp(&mod->namesym->namestr, "core") == 0)
        return;
    genlGpuSite(gen, inst, call);
}

// An atomic operation's pointer that is a constant: a global, or an address in
// one computed from constants (an element of a '@workgroup' array at a
// constant index). LLVM folds an address computation whose offset is zero
// into the global itself, and LLVM 23's SPIR-V backend, which types a pointer
// by the address computations made from it, crashes on an atomic operation
// through a pointer of another type (the global's struct or array, where the
// operation acts on its first number). So the address is made an instruction,
// stepping into first fields until it is the number's: one with a constant
// base and constant indices is made with a frozen index, which nothing folds,
// and the constant put back in its place.
static void genlGpuAtomicConstPtr(GenState *gen, LLVMValueRef inst, int ptrop, LLVMTypeRef used) {
    LLVMValueRef ptr = LLVMGetOperand(inst, ptrop);
    if (!LLVMIsAConstant(ptr))
        return;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    LLVMValueRef idx[64];
    int n = 0;
    LLVMValueRef base;
    LLVMTypeRef srctype, type;
    if (LLVMIsAGlobalVariable(ptr)) {
        base = ptr;
        srctype = type = LLVMGlobalGetValueType(ptr);
        idx[n++] = LLVMConstInt(i32, 0, 0);
    }
    else if (LLVMIsAConstantExpr(ptr) && LLVMGetConstOpcode(ptr) == LLVMGetElementPtr) {
        int nops = LLVMGetNumOperands(ptr);
        if (nops > 48)
            return;
        base = LLVMGetOperand(ptr, 0);
        srctype = type = LLVMGetGEPSourceElementType(ptr);
        for (int op = 1; op < nops; ++op) {
            idx[n++] = LLVMGetOperand(ptr, op);
            if (op == 1)
                continue;
            type = LLVMGetTypeKind(type) == LLVMStructTypeKind
                ? LLVMStructGetTypeAtIndex(type, (unsigned)LLVMConstIntGetZExtValue(idx[n - 1]))
                : LLVMGetElementType(type);
        }
    }
    else
        return;
    while (type != used && n < 60) {
        LLVMTypeKind kind = LLVMGetTypeKind(type);
        if (kind == LLVMStructTypeKind && LLVMCountStructElementTypes(type) > 0)
            type = LLVMStructGetTypeAtIndex(type, 0);
        else if (kind == LLVMArrayTypeKind)
            type = LLVMGetElementType(type);
        else
            return;     // Not a first number of it: left as it was
        idx[n++] = LLVMConstInt(i32, 0, 0);
    }
    if (n == 1)
        return;         // The global is the number itself
    // The first index steps the pointer and indexes no type, so the result's
    // type is known whatever it is
    LLVMPositionBuilderBefore(gen->builder, inst);
    LLVMValueRef first = idx[0];
    idx[0] = LLVMBuildFreeze(gen->builder, first, "");
    LLVMValueRef gep = LLVMBuildInBoundsGEP2(gen->builder, srctype, base, idx, n, "");
    LLVMSetOperand(gep, 1, first);
    LLVMInstructionEraseFromParent(idx[0]);
    LLVMSetOperand(inst, ptrop, gep);
}

// Settle each atomic instruction of a function on a GPU target: its scope from
// its memory, its ordering one Vulkan has. In a kernel, where everything is
// inlined and every pointer has its origin's space, one on memory that
// invocations do not share is refused; elsewhere (a library's function taking
// a reference) the memory is not known, and its scope is the device's.
// Answers 0 once it has refused one.
int genlGpuAtomics(GenState *gen, LLVMValueRef fn, FnDclNode *kernel) {
    unsigned device = LLVMGetSyncScopeID(gen->context, "device", 6);
    unsigned workgroup = LLVMGetSyncScopeID(gen->context, "workgroup", 9);
    unsigned system = LLVMGetSyncScopeID(gen->context, "", 0);
    int ok = 1;
    int kernelnamed = 0;
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        LLVMValueRef inst = LLVMGetFirstInstruction(blk);
        while (inst) {
            LLVMValueRef next = LLVMGetNextInstruction(inst);
            int ptrop;
            if (LLVMIsAAtomicRMWInst(inst) || LLVMIsAAtomicCmpXchgInst(inst) || (LLVMIsALoadInst(inst) && LLVMIsAtomic(inst)))
                ptrop = 0;
            else if (LLVMIsAStoreInst(inst) && LLVMIsAtomic(inst))
                ptrop = 1;
            else {
                inst = next;
                continue;
            }
            genlGpuAtomicConstPtr(gen, inst, ptrop, LLVMIsALoadInst(inst) ? LLVMTypeOf(inst)
                : LLVMIsAStoreInst(inst) ? LLVMTypeOf(LLVMGetOperand(inst, 0)) : LLVMTypeOf(LLVMGetOperand(inst, 1)));
            unsigned space = LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(inst, ptrop)));
            if (kernel && space != GenlStorageBufferSpace && space != GenlWorkgroup) {
                // Where the program wrote it, else once, at the kernel
                INode *site = genlGpuSiteOf(gen, inst);
                if (site || !kernelnamed)
                    errorMsgNode(site ? site : (INode *)kernel, ErrorGpuAtomicPlace,
                        "In kernel '%s', an atomic operation acts on memory that is each invocation's own: a local, or a global not '@workgroup'. On a GPU an atomic operation acts on memory invocations share, an element of a buffer the kernel binds or of a '@workgroup' global.",
                        &kernel->namesym->namestr);
                kernelnamed = kernelnamed || site == NULL;
                ok = 0;
                inst = next;
                continue;
            }
            if (LLVMIsAAtomicCmpXchgInst(inst)) {
                genlGpuCompareSwap(gen, inst, space);
                inst = next;
                continue;
            }
            // Only what LLVM left at the system scope: the error buffer's
            // count is made at the device's already
            if (LLVMGetAtomicSyncScopeID(inst) == system)
                LLVMSetAtomicSyncScopeID(inst, space == GenlWorkgroup ? workgroup : device);
            if (LLVMGetOrdering(inst) == LLVMAtomicOrderingSequentiallyConsistent)
                LLVMSetOrdering(inst, LLVMIsALoadInst(inst) ? LLVMAtomicOrderingAcquire
                    : LLVMIsAStoreInst(inst) ? LLVMAtomicOrderingRelease : LLVMAtomicOrderingAcquireRelease);
            inst = next;
        }
    }
    return ok;
}

// ---- The module, once emitted -----------------------------------------------

// SPIR-V's opcodes this reads or writes
enum {
    SpvOpTypeInt = 21, SpvOpConstant = 43, SpvOpCompositeInsert = 82, SpvOpCopyObject = 83,
    SpvOpAtomicLoad = 227, SpvOpAtomicStore = 228, SpvOpAtomicExchange = 229,
    SpvOpAtomicCompareExchange = 230, SpvOpAtomicXor = 242
};

// Where an atomic instruction's memory semantics operand is, or 0
static unsigned genlSpvSemanticsAt(uint32_t op) {
    if (op == SpvOpAtomicStore)
        return 3;
    if (op == SpvOpAtomicLoad || (op >= SpvOpAtomicExchange && op <= SpvOpAtomicXor && op != SpvOpAtomicCompareExchange))
        return 5;
    return 0;
}

// What LLVM 23's SPIR-V backend says wrongly about atomics, said rightly in the
// module it emitted. A relaxed atomic operation's semantics are made None, for
// the backend adds the storage class's bit to them, which Vulkan refuses
// without an ordering (a constant 0 is added if the module has none). And a
// compare-and-swap (genlGpuCompareSwap) is followed by two insertions of its
// result and its comparison into a value of the scalar type its intrinsic is
// declared with, which no module may hold: the first goes, and the second is
// made a copy of the result, which is what every use of it wants. 'words' is
// reallocated; answers its new count.
size_t genlGpuSyncPatch(uint32_t **wordsp, size_t nwords) {
    uint32_t *words = *wordsp;
    if (nwords < 5)
        return nwords;

    // The unsigned 32-bit type, and its constants' values by id
    uint32_t bound = words[3];
    uint32_t *value = (uint32_t *)calloc(bound, sizeof(uint32_t));
    uint8_t *isconst = (uint8_t *)calloc(bound, 1);
    uint8_t *isint = (uint8_t *)calloc(bound, 1);
    uint32_t *cxof = (uint32_t *)calloc(bound, sizeof(uint32_t));  // an insertion's: its compare-and-swap
    uint8_t *iscx = (uint8_t *)calloc(bound, 1);
    uint32_t uint32 = 0, zero = 0;
    size_t typeat = 0;
    int relaxed = 0;
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords)
            break;
        if (op == SpvOpTypeInt && count == 4 && words[i + 1] < bound) {
            isint[words[i + 1]] = 1;
            if (words[i + 2] == 32 && words[i + 3] == 0) {
                uint32 = words[i + 1];
                typeat = i + count;
            }
        }
        else if (op == SpvOpConstant && count == 4 && words[i + 1] == uint32 && uint32 && words[i + 2] < bound) {
            isconst[words[i + 2]] = 1;
            value[words[i + 2]] = words[i + 3];
            if (words[i + 3] == 0 && !zero)
                zero = words[i + 2];
        }
        else if (op == SpvOpAtomicCompareExchange && count >= 3 && words[i + 2] < bound)
            iscx[words[i + 2]] = 1;
        unsigned at = genlSpvSemanticsAt(op);
        if (at && at < count && words[i + at] < bound && isconst[words[i + at]]) {
            uint32_t sem = value[words[i + at]];
            relaxed = relaxed || (sem != 0 && (sem & (SpvSemAcquire | SpvSemRelease | SpvSemAcqRel | SpvSemSeqCst)) == 0);
        }
        i += count;
    }

    // A constant 0, made after the type when the module has none
    size_t extra = relaxed && !zero && typeat ? 4 : 0;
    uint32_t *out = (uint32_t *)malloc((nwords + extra) * sizeof(uint32_t));
    memcpy(out, words, 5 * sizeof(uint32_t));
    if (extra) {
        zero = bound;
        out[3] = bound + 1;
    }
    size_t o = 5;
    for (size_t i = 5; i < nwords;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > nwords) {
            memcpy(&out[o], &words[i], (nwords - i) * sizeof(uint32_t));
            o += nwords - i;
            break;
        }
        uint32_t *inst = &words[i];
        i += count;
        if (op == SpvOpCompositeInsert && count >= 5 && inst[1] < bound && isint[inst[1]] && inst[2] < bound) {
            if (inst[3] < bound && iscx[inst[3]]) {
                cxof[inst[2]] = inst[3];
                continue;
            }
            if (inst[4] < bound && cxof[inst[4]]) {
                out[o++] = (4u << 16) | SpvOpCopyObject;
                out[o++] = inst[1];
                out[o++] = inst[2];
                out[o++] = cxof[inst[4]];
                continue;
            }
        }
        memcpy(&out[o], inst, count * sizeof(uint32_t));
        unsigned at = genlSpvSemanticsAt(op);
        if (zero && at && at < count && inst[at] < bound && isconst[inst[at]]) {
            uint32_t sem = value[inst[at]];
            if (sem != 0 && (sem & (SpvSemAcquire | SpvSemRelease | SpvSemAcqRel | SpvSemSeqCst)) == 0)
                out[o + at] = zero;
        }
        o += count;
        if (extra && i == typeat) {
            out[o++] = (4u << 16) | SpvOpConstant;
            out[o++] = uint32;
            out[o++] = zero;
            out[o++] = 0;
        }
    }
    free(value);
    free(isconst);
    free(isint);
    free(cxof);
    free(iscx);
    free(words);
    *wordsp = out;
    return o;
}
