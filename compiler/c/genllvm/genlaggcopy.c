/** Large aggregates copied in memory, never carried as one value
 * @file
 *
 * Generation carries every struct, array and tuple as an LLVM first-class
 * aggregate value: a variable read is one 'load' of the whole of it, a struct
 * literal a chain of 'insertvalue's, a copy one 'store', and an argument or a
 * result is passed whole. LLVM's backend takes such a value apart element by
 * element (an 'Array[u64, 1024]' is 1024 values in instruction selection, and
 * a struct of seven of them 7,000), and the optimizer's instcombine splits a
 * load or store of one up to 1024 elements long, so that what one 'memcpy'
 * would do becomes thousands of instructions, each scheduled and allocated,
 * and a copy of an 'Array[i8, 100000]' crashes instruction selection.
 *
 * So once the module is generated, before anything is verified, printed or
 * optimized, every aggregate value larger than GenlAggCopyMin bytes is moved
 * into memory, the way clang generates C's structs: each such value gets a
 * home, an address holding it, and is copied with 'llvm.memcpy' (a null
 * constant with 'llvm.memset'):
 *
 * - a load copies what it reads into a slot of its own, at the load, so the
 *   value is what memory held there, whatever is stored later. A load whose
 *   only uses read parts of it, or store it once, with nothing between that
 *   could write memory, reads in place instead;
 * - an 'insertvalue' copies the aggregate it adds to (or takes its slot over,
 *   when it is that value's only use, in the same block) and stores the part
 *   into it;
 * - an 'extractvalue' is the part's address within its aggregate's home: a
 *   load of a part that is not large, the address itself of one that is;
 * - a store, and a return, copies from the value's home;
 * - a phi gets a slot, copied into at the end of each predecessor; a select
 *   chooses between two homes;
 * - a function taking a large value takes a pointer to its caller's copy,
 *   which it only reads; a function returning one fills the slot its caller
 *   passes first ('sret'), as clang's Win64 convention does for C. Every
 *   function, its declarations and every call to it, direct or through a
 *   vtable or reference, are changed together, so every object compiled by
 *   this compiler agrees. A C-named function keeps the C ABI genlcabi.c gives
 *   it: it is marked as it is declared ("cone-cabi"), and left alone.
 *
 * The same remaking gives a smaller aggregate result a register, since LLVM
 * returns a first-class aggregate one register per field, and on Win64 sends
 * a third and fourth float through the x87 stack (GenlRetClass): a struct of
 * 2 to 4 floats, or 2 doubles, returns as one vector; any other of up to 16
 * bytes, but one or two scalars, as one or two integers, through memory as
 * clang does; and one of 17 to 64 bytes through a slot its caller passes
 * ('sret'), loaded and stored whole. Arguments of 64 bytes or less are passed
 * as they were.
 *
 * A home is never written once its value exists: a slot is filled where its
 * value is made and only read after, which is what lets a part's address, a
 * select or a callee read it in place. Every copy is made at the point the
 * value it copies was loaded, stored, passed or returned, so what each sees
 * of memory, and the order of every call, every drop among them, is what it
 * was. Nothing here runs on a GPU target, where 'llvm.memcpy' is not legal
 * Vulkan SPIR-V and its own pipeline breaks every aggregate into scalars.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../coneopts.h"
#include "genllvm.h"

#include <llvm-c/Comdat.h>
#include <llvm-c/DebugInfo.h>

#include <stdlib.h>
#include <string.h>

// An aggregate of more than this many bytes is copied in memory. Carried
// whole, one of 24 to 136 bytes costs a debug build's code generation a few
// milliseconds more and a release build nothing measurable, and the cost
// grows past that faster than its size; at 64, every geomath type (a Mat4 is
// 64 bytes) is still carried whole, and its bench runs as it did, where at 16
// a Mat4 times a point ran 65% slower (generation.md, "Large aggregates")
#define GenlAggCopyMin 64

// The marker a C-named function carries from its declaration to here
#define GenlCAbiMarker "cone-cabi"

// An aggregate result of at most this many bytes, but a pair of scalars,
// returns in one or two registers as a vector or integers; a larger one up to
// GenlAggCopyMin through a slot its caller passes
#define GenlRetRegMax 16

// How a function returns its result
typedef enum {
    GenlRetWhole,   // as its own LLVM type: a scalar, or an aggregate of one or two scalars
    GenlRetVector,  // a struct of 2 to 4 floats or 2 doubles, as one vector
    GenlRetInt,     // any other aggregate of up to 16 bytes, as an integer or two
    GenlRetSlot,    // an aggregate of 17 to 64 bytes, stored whole in its caller's slot ('sret')
    GenlRetLarge,   // a large aggregate, copied from its home into its caller's slot ('sret')
} GenlRetClass;

// Where a large value lives
typedef struct {
    LLVMValueRef key;       // the value
    LLVMValueRef home;      // the address holding it
    unsigned align;         // the alignment that address is known to have
    int owned;              // a slot made for it alone, which an insertvalue that is its only use may fill in place
} GenlAggHome;

typedef struct {
    GenlAggHome *slots;
    uint32_t mask;
    uint32_t used;
} GenlAggHomes;

// The state of one function's rewriting
typedef struct {
    GenState *gen;
    LLVMValueRef fn;
    GenlRetClass retclass;  // how it returns its result
    LLVMValueRef sret;      // the slot its result is returned in ('sret'), or NULL
    LLVMValueRef at;        // the instruction new code is put before
    GenlAggHomes homes;
    LLVMValueRef *dead;     // instructions replaced, erased once all are
    uint32_t ndead, deadmax;
    LLVMValueRef *phis;     // large phis, given their slots before anything else
    uint32_t nphis, phimax;
    GenlAggHomes done;      // instructions already rewritten, by key alone (out of order, for a use before its definition)
} GenlAggFn;

static void genlAggPush(LLVMValueRef **list, uint32_t *cnt, uint32_t *max, LLVMValueRef val) {
    if (*cnt == *max) {
        *max = *max ? *max * 2 : 64;
        *list = (LLVMValueRef *)realloc(*list, *max * sizeof(LLVMValueRef));
    }
    (*list)[(*cnt)++] = val;
}

// Is a value of this type copied in memory?
static int genlAggIsLarge(GenState *gen, LLVMTypeRef type) {
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind)
        return 0;
    if (!LLVMTypeIsSized(type))
        return 0;
    return LLVMStoreSizeOfType(gen->datalayout, type) > GenlAggCopyMin;
}

static unsigned genlAggAlignOf(GenState *gen, LLVMTypeRef type) {
    return LLVMABIAlignmentOfType(gen->datalayout, type);
}

// ---- How a result returns -------------------------------------------------------

// The scalars an aggregate holds, all its parts' parts: how many (counting
// stops past 'max'), their bytes, and the one type they all have, or NULL
static void genlAggLeaves(GenState *gen, LLVMTypeRef type, unsigned max, unsigned *count,
        unsigned long long *bytes, LLVMTypeRef *same, int *mixed) {
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind) {
        unsigned cnt = kind == LLVMStructTypeKind
            ? LLVMCountStructElementTypes(type) : (unsigned)LLVMGetArrayLength2(type);
        for (unsigned i = 0; i < cnt && *count <= max; ++i) {
            LLVMTypeRef part = kind == LLVMStructTypeKind ? LLVMStructGetTypeAtIndex(type, i) : LLVMGetElementType(type);
            genlAggLeaves(gen, part, max, count, bytes, same, mixed);
        }
        return;
    }
    ++*count;
    *bytes += LLVMStoreSizeOfType(gen->datalayout, type);
    if (*same == NULL && !*mixed)
        *same = type;
    else if (*same != type) {
        *same = NULL;
        *mixed = 1;
    }
}

// How a function returning a value of 'type' returns it, and for a vector or
// integers, the LLVM type it returns instead ('coerced')
static GenlRetClass genlAggRetClass(GenState *gen, LLVMTypeRef type, LLVMTypeRef *coerced) {
    *coerced = NULL;
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if ((kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind) || !LLVMTypeIsSized(type))
        return GenlRetWhole;
    unsigned long long size = LLVMStoreSizeOfType(gen->datalayout, type);
    if (size > GenlAggCopyMin)
        return GenlRetLarge;
    if (size == 0)
        return GenlRetWhole;
    unsigned count = 0;
    unsigned long long bytes = 0;
    LLVMTypeRef same = NULL;
    int mixed = 0;
    genlAggLeaves(gen, type, GenlRetRegMax, &count, &bytes, &same, &mixed);
    if (size <= GenlRetRegMax) {
        // One scalar, or a pair (a slice's pointer and length): LLVM already
        // returns each in a register of its own, as Rust's ScalarPair does
        LLVMTypeKind samekind = same ? LLVMGetTypeKind(same) : LLVMVoidTypeKind;
        int floats = samekind == LLVMFloatTypeKind || samekind == LLVMDoubleTypeKind;
        if (count <= 1 || (count == 2 && !floats))
            return GenlRetWhole;
        // All one float type, with no padding: one vector register
        if (floats && bytes == size && (samekind == LLVMFloatTypeKind ? count <= 4 : count == 2)) {
            *coerced = LLVMVectorType(same, count);
            return GenlRetVector;
        }
        LLVMTypeRef i64 = LLVMInt64TypeInContext(gen->context);
        if (size > 8) {
            LLVMTypeRef pair[2] = { i64, i64 };
            *coerced = LLVMStructTypeInContext(gen->context, pair, 2, 0);
        }
        else
            *coerced = LLVMIntTypeInContext(gen->context, size <= 1 ? 8 : size <= 2 ? 16 : size <= 4 ? 32 : 64);
        return GenlRetInt;
    }
    return GenlRetSlot;
}

static GenlRetClass genlAggRetClassOf(GenState *gen, LLVMTypeRef type) {
    LLVMTypeRef coerced;
    return genlAggRetClass(gen, type, &coerced);
}

// Is a result of this class returned through a slot its caller passes?
static int genlAggRetSret(GenlRetClass retclass) {
    return retclass == GenlRetSlot || retclass == GenlRetLarge;
}

// ---- Homes, by value ---------------------------------------------------------

static GenlAggHome *genlAggFind(GenlAggHomes *map, LLVMValueRef key) {
    uint32_t i = (uint32_t)(((uintptr_t)key >> 4) * 2654435761u) & map->mask;
    while (map->slots[i].key && map->slots[i].key != key)
        i = (i + 1) & map->mask;
    return &map->slots[i];
}

static void genlAggPut(GenlAggHomes *map, LLVMValueRef key, LLVMValueRef home, unsigned align, int owned) {
    if ((map->used + 1) * 2 > map->mask + 1) {
        GenlAggHome *old = map->slots;
        uint32_t oldcap = map->mask + 1;
        map->mask = oldcap * 2 - 1;
        map->slots = (GenlAggHome *)calloc(oldcap * 2, sizeof(GenlAggHome));
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (old[i].key)
                *genlAggFind(map, old[i].key) = old[i];
        }
        free(old);
    }
    GenlAggHome *slot = genlAggFind(map, key);
    if (!slot->key)
        ++map->used;
    slot->key = key;
    slot->home = home;
    slot->align = align;
    slot->owned = owned;
}

// ---- Building ---------------------------------------------------------------

// A slot of 'type' in the function's frame, new code staying where it was
static LLVMValueRef genlAggSlot(GenlAggFn *st, LLVMTypeRef type) {
    GenState *gen = st->gen;
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(st->fn);
    LLVMMetadataRef loc = LLVMGetCurrentDebugLocation2(gen->builder);
    LLVMSetCurrentDebugLocation2(gen->builder, NULL);
    LLVMPositionBuilder(gen->builder, entry, LLVMGetFirstInstruction(entry));
    LLVMValueRef slot = LLVMBuildAlloca(gen->builder, type, "agg");
    LLVMSetAlignment(slot, genlAggAlignOf(gen, type));
    LLVMPositionBuilderBefore(gen->builder, st->at);
    LLVMSetCurrentDebugLocation2(gen->builder, loc);
    return slot;
}

// Put new code before 'inst', at its source location
static void genlAggAt(GenlAggFn *st, LLVMValueRef inst) {
    st->at = inst;
    LLVMPositionBuilderBefore(st->gen->builder, inst);
    LLVMSetCurrentDebugLocation2(st->gen->builder, LLVMInstructionGetDebugLoc(inst));
}

static LLVMValueRef genlAggSize(GenState *gen, LLVMTypeRef type) {
    return LLVMConstInt(LLVMInt64TypeInContext(gen->context), LLVMStoreSizeOfType(gen->datalayout, type), 0);
}

// The largest power of two dividing 'offset', no more than 'align'
static unsigned genlAggOffsetAlign(unsigned align, unsigned long long offset) {
    if (offset == 0)
        return align;
    unsigned long long low = offset & (~offset + 1);
    return low < align ? (unsigned)low : align;
}

// The address of the part an index path names within the aggregate of type
// 'type' at 'base', its type, and the alignment it is known to have
static LLVMValueRef genlAggPart(GenlAggFn *st, LLVMValueRef base, unsigned basealign, LLVMTypeRef type,
        const unsigned *idx, unsigned nidx, LLVMTypeRef *parttype, unsigned *partalign) {
    GenState *gen = st->gen;
    LLVMValueRef *gepidx = (LLVMValueRef *)malloc((nidx + 1) * sizeof(LLVMValueRef));
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(gen->context);
    gepidx[0] = LLVMConstInt(i32, 0, 0);
    unsigned long long offset = 0;
    LLVMTypeRef part = type;
    for (unsigned d = 0; d < nidx; ++d) {
        if (LLVMGetTypeKind(part) == LLVMStructTypeKind) {
            offset += LLVMOffsetOfElement(gen->datalayout, part, idx[d]);
            gepidx[d + 1] = LLVMConstInt(i32, idx[d], 0);
            part = LLVMStructGetTypeAtIndex(part, idx[d]);
        }
        else {
            part = LLVMGetElementType(part);
            offset += LLVMABISizeOfType(gen->datalayout, part) * idx[d];
            gepidx[d + 1] = LLVMConstInt(i64, idx[d], 0);
        }
    }
    LLVMValueRef addr = LLVMBuildGEP2(gen->builder, type, base, gepidx, nidx + 1, "aggpart");
    free(gepidx);
    *parttype = part;
    *partalign = genlAggOffsetAlign(basealign, offset);
    return addr;
}

static GenlAggHome *genlAggHomeOf(GenlAggFn *st, LLVMValueRef val);

// Is every part of a constant null or undefined?
static int genlAggConstBlank(LLVMValueRef val) {
    if (LLVMIsUndef(val) || LLVMIsPoison(val) || LLVMIsNull(val))
        return 1;
    LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(val));
    if (kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind)
        return 0;
    unsigned cnt = kind == LLVMStructTypeKind
        ? LLVMCountStructElementTypes(LLVMTypeOf(val)) : (unsigned)LLVMGetArrayLength2(LLVMTypeOf(val));
    for (unsigned i = 0; i < cnt; ++i) {
        if (!genlAggConstBlank(LLVMGetAggregateElement(val, i)))
            return 0;
    }
    return 1;
}

// Store the large constant 'val' at 'dest': nothing for undef, a memset where
// every part is null or undefined, each field of a struct in turn, and any
// other array copied from a private constant of it (one per constant, kept in
// 'st->gen's module by genlAggCopies)
static GenlAggHomes *genlAggConsts;

static void genlAggConstTo(GenlAggFn *st, LLVMValueRef dest, unsigned align, LLVMValueRef val) {
    GenState *gen = st->gen;
    LLVMTypeRef type = LLVMTypeOf(val);
    if (LLVMIsUndef(val) || LLVMIsPoison(val))
        return;
    if (genlAggConstBlank(val)) {
        LLVMBuildMemSet(gen->builder, dest, LLVMConstInt(LLVMInt8TypeInContext(gen->context), 0, 0),
            genlAggSize(gen, type), align);
        return;
    }
    if (LLVMGetTypeKind(type) == LLVMStructTypeKind) {
        unsigned cnt = LLVMCountStructElementTypes(type);
        for (unsigned i = 0; i < cnt; ++i) {
            LLVMValueRef elem = LLVMGetAggregateElement(val, i);
            if (LLVMIsUndef(elem) || LLVMIsPoison(elem))
                continue;
            LLVMTypeRef parttype;
            unsigned partalign;
            LLVMValueRef addr = genlAggPart(st, dest, align, type, &i, 1, &parttype, &partalign);
            if (genlAggIsLarge(gen, parttype))
                genlAggConstTo(st, addr, partalign, elem);
            else {
                LLVMValueRef store = LLVMBuildStore(gen->builder, elem, addr);
                LLVMSetAlignment(store, partalign);
            }
        }
        return;
    }
    GenlAggHome *found = genlAggFind(genlAggConsts, val);
    if (!found->key) {
        LLVMValueRef global = LLVMAddGlobal(gen->module, type, "aggconst");
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
        LLVMSetInitializer(global, val);
        LLVMSetAlignment(global, genlAggAlignOf(gen, type));
        genlAggPut(genlAggConsts, val, global, genlAggAlignOf(gen, type), 0);
        found = genlAggFind(genlAggConsts, val);
    }
    LLVMBuildMemCpy(gen->builder, dest, align, found->home, found->align, genlAggSize(gen, type));
}

// A large constant's home, at the current position: a fresh slot it is stored in
static GenlAggHome genlAggConstHome(GenlAggFn *st, LLVMValueRef val) {
    LLVMTypeRef type = LLVMTypeOf(val);
    GenlAggHome home = { val, genlAggSlot(st, type), genlAggAlignOf(st->gen, type), 0 };
    genlAggConstTo(st, home.home, home.align, val);
    return home;
}

// Copy the large value 'val' to 'dest', known to be aligned to 'align'
static void genlAggCopyTo(GenlAggFn *st, LLVMValueRef dest, unsigned align, LLVMValueRef val) {
    GenState *gen = st->gen;
    LLVMTypeRef type = LLVMTypeOf(val);
    if (LLVMIsConstant(val)) {
        genlAggConstTo(st, dest, align, val);
        return;
    }
    GenlAggHome *src = genlAggHomeOf(st, val);
    if (src->home == dest)
        return;
    LLVMBuildMemCpy(gen->builder, dest, align, src->home, src->align, genlAggSize(gen, type));
}

// Store a part, large or not, at 'dest'
static void genlAggStorePart(GenlAggFn *st, LLVMValueRef dest, unsigned align, LLVMValueRef val) {
    if (genlAggIsLarge(st->gen, LLVMTypeOf(val))) {
        genlAggCopyTo(st, dest, align, val);
        return;
    }
    LLVMValueRef store = LLVMBuildStore(st->gen->builder, val, dest);
    LLVMSetAlignment(store, align);
}

static void genlAggDead(GenlAggFn *st, LLVMValueRef inst) {
    genlAggPush(&st->dead, &st->ndead, &st->deadmax, inst);
}

// ---- What may write memory --------------------------------------------------

static int genlAggWrites(LLVMValueRef inst) {
    switch (LLVMGetInstructionOpcode(inst)) {
    case LLVMStore: case LLVMCall: case LLVMInvoke: case LLVMCallBr:
    case LLVMAtomicRMW: case LLVMAtomicCmpXchg: case LLVMFence: case LLVMVAArg:
        return 1;
    default:
        return 0;
    }
}

// Is 'user' a later instruction of 'from's block with nothing between that may
// write memory?
static int genlAggClearTo(LLVMValueRef from, LLVMValueRef user) {
    if (!LLVMIsAInstruction(user) || LLVMGetInstructionParent(user) != LLVMGetInstructionParent(from))
        return 0;
    for (LLVMValueRef inst = LLVMGetNextInstruction(from); inst; inst = LLVMGetNextInstruction(inst)) {
        if (inst == user)
            return 1;
        if (genlAggWrites(inst))
            return 0;
    }
    return 0;
}

// Can a large value loaded by 'load' be read where it lies, every use of it
// coming before anything could change it: each a read of a part (of a part),
// or one store or return of it?
static int genlAggReadInPlace(GenState *gen, LLVMValueRef load, LLVMValueRef val) {
    for (LLVMUseRef use = LLVMGetFirstUse(val); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (!genlAggClearTo(load, user))
            return 0;
        if (LLVMIsAExtractValueInst(user)) {
            // A large part is read where it lies too, so its uses must be
            if (genlAggIsLarge(gen, LLVMTypeOf(user)) && !genlAggReadInPlace(gen, load, user))
                return 0;
        }
        else if (LLVMIsAStoreInst(user)) {
            if (LLVMGetOperand(user, 0) != val || LLVMGetVolatile(user))
                return 0;
        }
        else if (!LLVMIsAReturnInst(user))
            return 0;
    }
    return 1;
}

// ---- Calls and signatures -----------------------------------------------------

static int genlAggIsCAbi(LLVMValueRef callee) {
    return callee && LLVMIsAFunction(callee)
        && LLVMGetStringAttributeAtIndex(callee, LLVMAttributeFunctionIndex, GenlCAbiMarker,
            (unsigned)strlen(GenlCAbiMarker)) != NULL;
}

// Does a function type take or return a large value?
static int genlAggSigLarge(GenState *gen, LLVMTypeRef fntype) {
    if (genlAggIsLarge(gen, LLVMGetReturnType(fntype)))
        return 1;
    unsigned nparms = LLVMCountParamTypes(fntype);
    LLVMTypeRef *parms = (LLVMTypeRef *)malloc((nparms + 1) * sizeof(LLVMTypeRef));
    LLVMGetParamTypes(fntype, parms);
    int large = 0;
    for (unsigned i = 0; i < nparms && !large; ++i)
        large = genlAggIsLarge(gen, parms[i]);
    free(parms);
    return large;
}

// Does Cone's convention change a function of this type: does it take or
// return a large value, or return a smaller aggregate otherwise than whole?
static int genlAggSigChanged(GenState *gen, LLVMTypeRef fntype) {
    return genlAggRetClassOf(gen, LLVMGetReturnType(fntype)) != GenlRetWhole || genlAggSigLarge(gen, fntype);
}

// Is this a call LLVM itself defines, whose type no convention changes?
static int genlAggIsIntrinsic(LLVMValueRef callee) {
    return callee && LLVMIsAFunction(callee) && LLVMGetIntrinsicID(callee) != 0;
}

// The function type taking a pointer for each large parameter, and returning
// its result as genlAggRetClass says: through a slot passed first, as a
// vector or integers, or as it is
static LLVMTypeRef genlAggSig(GenState *gen, LLVMTypeRef fntype) {
    LLVMTypeRef ptr = LLVMPointerTypeInContext(gen->context, 0);
    LLVMTypeRef rettype = LLVMGetReturnType(fntype);
    LLVMTypeRef coerced;
    GenlRetClass retclass = genlAggRetClass(gen, rettype, &coerced);
    int sret = genlAggRetSret(retclass);
    unsigned nparms = LLVMCountParamTypes(fntype);
    LLVMTypeRef *parms = (LLVMTypeRef *)malloc((nparms + 2) * sizeof(LLVMTypeRef));
    LLVMGetParamTypes(fntype, parms + sret);
    if (sret) {
        parms[0] = ptr;
        rettype = LLVMVoidTypeInContext(gen->context);
    }
    else if (coerced)
        rettype = coerced;
    for (unsigned i = sret; i < nparms + sret; ++i) {
        if (genlAggIsLarge(gen, parms[i]))
            parms[i] = ptr;
    }
    LLVMTypeRef newtype = LLVMFunctionType(rettype, parms, nparms + sret, LLVMIsFunctionVarArg(fntype));
    free(parms);
    return newtype;
}

static LLVMAttributeRef genlAggEnumAttr(GenState *gen, const char *name) {
    return LLVMCreateEnumAttribute(gen->context, LLVMGetEnumAttributeKindForName(name, strlen(name)), 0);
}

static LLVMAttributeRef genlAggSretAttr(GenState *gen, LLVMTypeRef type) {
    return LLVMCreateTypeAttribute(gen->context, LLVMGetEnumAttributeKindForName("sret", 4), type);
}

// Copy the attributes at 'from' of a function (or call, 'call') to index 'to' of another
static void genlAggCopyAttrs(LLVMValueRef from, LLVMValueRef to, LLVMAttributeIndex fromidx,
        LLVMAttributeIndex toidx, int call) {
    unsigned cnt = call ? LLVMGetCallSiteAttributeCount(from, fromidx) : LLVMGetAttributeCountAtIndex(from, fromidx);
    if (cnt == 0)
        return;
    LLVMAttributeRef *attrs = (LLVMAttributeRef *)malloc(cnt * sizeof(LLVMAttributeRef));
    if (call)
        LLVMGetCallSiteAttributes(from, fromidx, attrs);
    else
        LLVMGetAttributesAtIndex(from, fromidx, attrs);
    for (unsigned i = 0; i < cnt; ++i) {
        if (call)
            LLVMAddCallSiteAttribute(to, toidx, attrs[i]);
        else
            LLVMAddAttributeAtIndex(to, toidx, attrs[i]);
    }
    free(attrs);
}

// The attributes of a function or call whose type genlAggSig changed: the
// result slot's, then each parameter's, a large one's its own. A result
// returned as a vector or integers keeps none of its own
static void genlAggSigAttrs(GenState *gen, LLVMValueRef from, LLVMValueRef to, LLVMTypeRef oldtype, int call) {
    LLVMTypeRef rettype = LLVMGetReturnType(oldtype);
    GenlRetClass retclass = genlAggRetClassOf(gen, rettype);
    int sret = genlAggRetSret(retclass);
    genlAggCopyAttrs(from, to, LLVMAttributeFunctionIndex, LLVMAttributeFunctionIndex, call);
    if (sret) {
        LLVMAttributeRef attrs[2] = { genlAggSretAttr(gen, rettype), genlAggEnumAttr(gen, "noalias") };
        for (int a = 0; a < 2; ++a) {
            if (call)
                LLVMAddCallSiteAttribute(to, 1, attrs[a]);
            else
                LLVMAddAttributeAtIndex(to, 1, attrs[a]);
        }
    }
    else if (retclass == GenlRetWhole)
        genlAggCopyAttrs(from, to, LLVMAttributeReturnIndex, LLVMAttributeReturnIndex, call);
    unsigned nparms = LLVMCountParamTypes(oldtype);
    LLVMTypeRef *parms = (LLVMTypeRef *)malloc((nparms + 1) * sizeof(LLVMTypeRef));
    LLVMGetParamTypes(oldtype, parms);
    for (unsigned i = 0; i < nparms; ++i) {
        if (!genlAggIsLarge(gen, parms[i]))
            genlAggCopyAttrs(from, to, i + 1, i + 1 + sret, call);
        else if (!call) {
            // Only read, and the caller's copy is its own
            LLVMAddAttributeAtIndex(to, i + 1 + sret, genlAggEnumAttr(gen, "noalias"));
            LLVMAddAttributeAtIndex(to, i + 1 + sret, genlAggEnumAttr(gen, "readonly"));
        }
    }
    free(parms);
}

// Remake a function genlAggSigChanged names with the type genlAggSig
// gives it, under the same name, with its body; its large parameters' homes
// are the new pointer parameters. The old function is left, empty and
// unnamed, its large parameters still used, for genlAggFns to delete
static LLVMValueRef genlAggRetype(GenState *gen, LLVMValueRef fn, GenlAggHomes *params) {
    LLVMTypeRef oldtype = LLVMGlobalGetValueType(fn);
    LLVMTypeRef newtype = genlAggSig(gen, oldtype);
    int sret = genlAggRetSret(genlAggRetClassOf(gen, LLVMGetReturnType(oldtype)));
    size_t namelen;
    const char *oldname = LLVMGetValueName2(fn, &namelen);
    char *name = (char *)malloc(namelen + 1);
    memcpy(name, oldname, namelen);
    name[namelen] = '\0';
    LLVMSetValueName2(fn, "", 0);
    LLVMValueRef newfn = LLVMAddFunction(gen->module, name, newtype);
    free(name);

    LLVMSetLinkage(newfn, LLVMGetLinkage(fn));
    LLVMSetVisibility(newfn, LLVMGetVisibility(fn));
    LLVMSetDLLStorageClass(newfn, LLVMGetDLLStorageClass(fn));
    LLVMSetUnnamedAddress(newfn, LLVMGetUnnamedAddress(fn));
    LLVMSetFunctionCallConv(newfn, LLVMGetFunctionCallConv(fn));
    if (LLVMGetComdat(fn))
        LLVMSetComdat(newfn, LLVMGetComdat(fn));
    if (LLVMGetSection(fn))
        LLVMSetSection(newfn, LLVMGetSection(fn));
    if (LLVMGetAlignment(fn))
        LLVMSetAlignment(newfn, LLVMGetAlignment(fn));
    if (LLVMGetGC(fn))
        LLVMSetGC(newfn, LLVMGetGC(fn));
    if (LLVMHasPersonalityFn(fn))
        LLVMSetPersonalityFn(newfn, LLVMGetPersonalityFn(fn));
    genlAggSigAttrs(gen, fn, newfn, oldtype, 0);
    // Its debug information and other metadata move with it: a subprogram
    // belongs to one function
    size_t nmeta;
    LLVMValueMetadataEntry *meta = LLVMGlobalCopyAllMetadata(fn, &nmeta);
    for (size_t m = 0; m < nmeta; ++m)
        LLVMGlobalSetMetadata(newfn, LLVMValueMetadataEntriesGetKind(meta, (unsigned)m),
            LLVMValueMetadataEntriesGetMetadata(meta, (unsigned)m));
    if (meta)
        LLVMDisposeValueMetadataEntries(meta);
    LLVMGlobalClearMetadata(fn);

    LLVMBasicBlockRef blk;
    while ((blk = LLVMGetFirstBasicBlock(fn))) {
        LLVMRemoveBasicBlockFromParent(blk);
        LLVMAppendExistingBasicBlock(newfn, blk);
    }
    if (sret)
        LLVMSetValueName2(LLVMGetParam(newfn, 0), "sret", 4);
    unsigned nparms = LLVMCountParams(fn);
    for (unsigned i = 0; i < nparms; ++i) {
        LLVMValueRef oldparm = LLVMGetParam(fn, i);
        LLVMValueRef newparm = LLVMGetParam(newfn, i + sret);
        size_t len;
        const char *pname = LLVMGetValueName2(oldparm, &len);
        LLVMSetValueName2(newparm, pname, len);
        LLVMTypeRef ptype = LLVMTypeOf(oldparm);
        if (genlAggIsLarge(gen, ptype))
            genlAggPut(params, oldparm, newparm, genlAggAlignOf(gen, ptype), 0);
        else
            LLVMReplaceAllUsesWith(oldparm, newparm);
    }
    LLVMReplaceAllUsesWith(fn, newfn);
    return newfn;
}

// Each float of 'val', a small aggregate of one float type, put in turn into
// the vector 'vec' from its 'n'th element
static LLVMValueRef genlAggToVector(GenState *gen, LLVMValueRef val, LLVMValueRef vec, unsigned *n) {
    LLVMTypeRef type = LLVMTypeOf(val);
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind) {
        unsigned cnt = kind == LLVMStructTypeKind
            ? LLVMCountStructElementTypes(type) : (unsigned)LLVMGetArrayLength2(type);
        for (unsigned i = 0; i < cnt; ++i)
            vec = genlAggToVector(gen, LLVMBuildExtractValue(gen->builder, val, i, ""), vec, n);
        return vec;
    }
    LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(gen->context), (*n)++, 0);
    return LLVMBuildInsertElement(gen->builder, vec, val, idx, "");
}

// The aggregate of type 'type' made of the vector 'vec's floats from its 'n'th
static LLVMValueRef genlAggFromVector(GenState *gen, LLVMValueRef vec, LLVMTypeRef type, unsigned *n) {
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind) {
        unsigned cnt = kind == LLVMStructTypeKind
            ? LLVMCountStructElementTypes(type) : (unsigned)LLVMGetArrayLength2(type);
        LLVMValueRef agg = LLVMGetPoison(type);
        for (unsigned i = 0; i < cnt; ++i) {
            LLVMTypeRef part = kind == LLVMStructTypeKind ? LLVMStructGetTypeAtIndex(type, i) : LLVMGetElementType(type);
            agg = LLVMBuildInsertValue(gen->builder, agg, genlAggFromVector(gen, vec, part, n), i, "");
        }
        return agg;
    }
    LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(gen->context), (*n)++, 0);
    return LLVMBuildExtractElement(gen->builder, vec, idx, "");
}

// 'val' of type 'from' as type 'to', through a slot of the function's frame,
// as clang coerces a C struct to integers: an aggregate stored and integers
// loaded ('toint'), or the reverse. For integers, the slot is zeroed first
// where the aggregate's scalars leave a byte of them (padding, or the bytes
// past its end), so that no integer loaded holds an undefined byte
static LLVMValueRef genlAggRecast(GenlAggFn *st, LLVMValueRef val, LLVMTypeRef from, LLVMTypeRef to, int toint) {
    GenState *gen = st->gen;
    unsigned long long fromsize = LLVMStoreSizeOfType(gen->datalayout, from);
    unsigned long long tosize = LLVMStoreSizeOfType(gen->datalayout, to);
    LLVMTypeRef wide = tosize > fromsize ? to : from;
    unsigned align = genlAggAlignOf(gen, from) > genlAggAlignOf(gen, to) ? genlAggAlignOf(gen, from) : genlAggAlignOf(gen, to);
    LLVMValueRef slot = genlAggSlot(st, wide);
    if (LLVMGetAlignment(slot) < align)
        LLVMSetAlignment(slot, align);
    if (toint) {
        unsigned count = 0;
        unsigned long long bytes = 0;
        LLVMTypeRef same = NULL;
        int mixed = 0;
        genlAggLeaves(gen, from, ~0u - 1, &count, &bytes, &same, &mixed);
        if (bytes < tosize) {
            LLVMValueRef zero = LLVMBuildStore(gen->builder, LLVMConstNull(to), slot);
            LLVMSetAlignment(zero, align);
        }
    }
    LLVMValueRef store = LLVMBuildStore(gen->builder, val, slot);
    LLVMSetAlignment(store, genlAggAlignOf(gen, from));
    LLVMValueRef load = LLVMBuildLoad2(gen->builder, to, slot, "");
    LLVMSetAlignment(load, genlAggAlignOf(gen, to));
    return load;
}

// A call taking a large value or returning an aggregate, remade to
// genlAggSig's type with each large argument's home, and a slot for a result
// returned through one
static void genlAggCall(GenlAggFn *st, LLVMValueRef call) {
    GenState *gen = st->gen;
    LLVMTypeRef oldtype = LLVMGetCalledFunctionType(call);
    LLVMTypeRef newtype = genlAggSig(gen, oldtype);
    LLVMTypeRef rettype = LLVMGetReturnType(oldtype);
    LLVMTypeRef coerced;
    GenlRetClass retclass = genlAggRetClass(gen, rettype, &coerced);
    int sret = genlAggRetSret(retclass);
    unsigned nargs = LLVMGetNumArgOperands(call);
    LLVMValueRef *args = (LLVMValueRef *)malloc((nargs + 2) * sizeof(LLVMValueRef));
    LLVMValueRef slot = NULL;
    if (sret)
        args[0] = slot = genlAggSlot(st, rettype);
    for (unsigned i = 0; i < nargs; ++i) {
        LLVMValueRef arg = LLVMGetOperand(call, i);
        args[i + sret] = genlAggIsLarge(gen, LLVMTypeOf(arg)) ? genlAggHomeOf(st, arg)->home : arg;
    }
    LLVMValueRef newcall = LLVMBuildCall2(gen->builder, newtype, LLVMGetCalledValue(call), args, nargs + sret, "");
    free(args);
    LLVMSetInstructionCallConv(newcall, LLVMGetInstructionCallConv(call));
    genlAggSigAttrs(gen, call, newcall, oldtype, 1);
    switch (retclass) {
    case GenlRetLarge:
        genlAggPut(&st->homes, call, slot, genlAggAlignOf(gen, rettype), 1);
        break;
    case GenlRetSlot: {
        LLVMValueRef load = LLVMBuildLoad2(gen->builder, rettype, slot, "");
        LLVMSetAlignment(load, genlAggAlignOf(gen, rettype));
        LLVMReplaceAllUsesWith(call, load);
        break;
    }
    case GenlRetVector: {
        unsigned n = 0;
        LLVMReplaceAllUsesWith(call, genlAggFromVector(gen, newcall, rettype, &n));
        break;
    }
    case GenlRetInt:
        LLVMReplaceAllUsesWith(call, genlAggRecast(st, newcall, coerced, rettype, 0));
        break;
    default:
        if (LLVMGetTypeKind(rettype) != LLVMVoidTypeKind)
            LLVMReplaceAllUsesWith(call, newcall);
        break;
    }
    genlAggDead(st, call);
}

// ---- Each instruction ---------------------------------------------------------

static int genlAggIsDone(GenlAggFn *st, LLVMValueRef inst) {
    return genlAggFind(&st->done, inst)->key != NULL;
}

static void genlAggInst(GenlAggFn *st, LLVMValueRef inst);

// The home of a large value: a constant's made here, an instruction's made
// where it is, first if it has not been yet
static GenlAggHome *genlAggHomeOf(GenlAggFn *st, LLVMValueRef val) {
    GenlAggHome *home = genlAggFind(&st->homes, val);
    if (home->key)
        return home;
    if (LLVMIsAConstant(val)) {
        // A slot is made for each use, where it is
        static GenlAggHome here;
        here = genlAggConstHome(st, val);
        return &here;
    }
    if (LLVMIsAInstruction(val) && !genlAggIsDone(st, val)) {
        // Only in code nothing reaches, where a use may come first
        LLVMValueRef at = st->at;
        genlAggInst(st, val);
        genlAggAt(st, at);
        home = genlAggFind(&st->homes, val);
        if (home->key)
            return home;
    }
    // A large parameter of a function keeping the C ABI (SysV, not built),
    // or anything unforeseen: stored whole into a slot of its own, as before
    LLVMValueRef at = st->at;
    LLVMValueRef slot = genlAggSlot(st, LLVMTypeOf(val));
    if (LLVMIsAArgument(val)) {
        LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(st->fn);
        LLVMValueRef first = LLVMGetFirstInstruction(entry);
        while (first && LLVMIsAAllocaInst(first))
            first = LLVMGetNextInstruction(first);
        LLVMPositionBuilderBefore(st->gen->builder, first);
    }
    else
        LLVMPositionBuilder(st->gen->builder, LLVMGetInstructionParent(val), LLVMGetNextInstruction(val));
    LLVMBuildStore(st->gen->builder, val, slot);
    genlAggAt(st, at);
    genlAggPut(&st->homes, val, slot, genlAggAlignOf(st->gen, LLVMTypeOf(val)), 1);
    return genlAggFind(&st->homes, val);
}

// Each large operand of an instruction nothing here rewrites is loaded whole
// from its home just before it, as it was before
static void genlAggLoadOperands(GenlAggFn *st, LLVMValueRef inst) {
    int nops = LLVMGetNumOperands(inst);
    for (int op = 0; op < nops; ++op) {
        LLVMValueRef val = LLVMGetOperand(inst, op);
        if (LLVMIsABasicBlock(val) || !genlAggIsLarge(st->gen, LLVMTypeOf(val)))
            continue;
        if (LLVMIsUndef(val) || LLVMIsPoison(val))
            continue;
        GenlAggHome *home = genlAggHomeOf(st, val);
        LLVMValueRef load = LLVMBuildLoad2(st->gen->builder, LLVMTypeOf(val), home->home, "");
        LLVMSetAlignment(load, home->align);
        // Its use here is gone, so its slot is no longer its alone to fill in place
        home->owned = 0;
        LLVMSetOperand(inst, op, load);
    }
}

static void genlAggInst(GenlAggFn *st, LLVMValueRef inst) {
    GenState *gen = st->gen;
    genlAggPut(&st->done, inst, NULL, 0, 0);
    LLVMTypeRef type = LLVMTypeOf(inst);
    int large = genlAggIsLarge(gen, type);
    genlAggAt(st, inst);

    switch (LLVMGetInstructionOpcode(inst)) {
    case LLVMCall: {
        LLVMValueRef callee = LLVMGetCalledValue(inst);
        LLVMTypeRef fntype = LLVMGetCalledFunctionType(inst);
        if (genlAggIsIntrinsic(callee) || !genlAggSigChanged(gen, fntype))
            return;
        if (!genlAggIsCAbi(callee)) {
            genlAggCall(st, inst);
            return;
        }
        // A C-named function keeps its own convention
        if (!genlAggSigLarge(gen, fntype))
            return;
        genlAggLoadOperands(st, inst);
        if (large)
            genlAggHomeOf(st, inst);
        return;
    }
    case LLVMLoad:
        if (!large)
            return;
        if (!LLVMGetVolatile(inst) && genlAggReadInPlace(gen, inst, inst)) {
            genlAggPut(&st->homes, inst, LLVMGetOperand(inst, 0), LLVMGetAlignment(inst), 0);
        }
        else {
            LLVMValueRef slot = genlAggSlot(st, type);
            LLVMBuildMemCpy(gen->builder, slot, genlAggAlignOf(gen, type),
                LLVMGetOperand(inst, 0), LLVMGetAlignment(inst), genlAggSize(gen, type));
            genlAggPut(&st->homes, inst, slot, genlAggAlignOf(gen, type), 1);
        }
        genlAggDead(st, inst);
        return;
    case LLVMStore: {
        LLVMValueRef val = LLVMGetOperand(inst, 0);
        if (!genlAggIsLarge(gen, LLVMTypeOf(val)))
            return;
        if (LLVMGetVolatile(inst)) {
            genlAggLoadOperands(st, inst);
            return;
        }
        genlAggCopyTo(st, LLVMGetOperand(inst, 1), LLVMGetAlignment(inst), val);
        genlAggDead(st, inst);
        return;
    }
    case LLVMInsertValue: {
        if (!large)
            return;
        LLVMValueRef agg = LLVMGetOperand(inst, 0);
        LLVMValueRef part = LLVMGetOperand(inst, 1);
        LLVMValueRef home;
        unsigned align = genlAggAlignOf(gen, type);
        GenlAggHome *from = NULL;
        if (!LLVMIsConstant(agg))
            from = genlAggHomeOf(st, agg);
        // The aggregate added to is filled in place when this is its only use,
        // in the block that made it, so it cannot be used again by a later
        // pass of a loop. (A use replaced by a load of it whole leaves it
        // not 'owned', genlAggLoadOperands.)
        if (from && from->owned && LLVMGetFirstUse(agg) && !LLVMGetNextUse(LLVMGetFirstUse(agg))
                && LLVMGetInstructionParent(agg) == LLVMGetInstructionParent(inst)) {
            home = from->home;
            align = from->align;
        }
        else {
            home = genlAggSlot(st, type);
            genlAggCopyTo(st, home, align, agg);
        }
        LLVMTypeRef parttype;
        unsigned partalign;
        LLVMValueRef addr = genlAggPart(st, home, align, type, LLVMGetIndices(inst), LLVMGetNumIndices(inst),
            &parttype, &partalign);
        genlAggStorePart(st, addr, partalign, part);
        genlAggPut(&st->homes, inst, home, align, 1);
        genlAggDead(st, inst);
        return;
    }
    case LLVMExtractValue: {
        LLVMValueRef agg = LLVMGetOperand(inst, 0);
        LLVMTypeRef aggtype = LLVMTypeOf(agg);
        if (!genlAggIsLarge(gen, aggtype))
            return;
        GenlAggHome *from = genlAggHomeOf(st, agg);
        LLVMValueRef base = from->home;
        unsigned basealign = from->align;
        LLVMTypeRef parttype;
        unsigned partalign;
        LLVMValueRef addr = genlAggPart(st, base, basealign, aggtype, LLVMGetIndices(inst), LLVMGetNumIndices(inst),
            &parttype, &partalign);
        if (large)
            genlAggPut(&st->homes, inst, addr, partalign, 0);
        else {
            LLVMValueRef load = LLVMBuildLoad2(gen->builder, parttype, addr, "");
            LLVMSetAlignment(load, partalign);
            LLVMReplaceAllUsesWith(inst, load);
        }
        genlAggDead(st, inst);
        return;
    }
    case LLVMRet: {
        if (LLVMGetNumOperands(inst) == 0)
            return;
        LLVMValueRef val = LLVMGetOperand(inst, 0);
        LLVMTypeRef valtype = LLVMTypeOf(val);
        if (!genlAggIsLarge(gen, valtype)) {
            // A smaller aggregate, as its function's type now returns it
            LLVMTypeRef coerced;
            if (genlAggRetClass(gen, valtype, &coerced) != st->retclass)
                return;
            switch (st->retclass) {
            case GenlRetSlot: {
                LLVMValueRef store = LLVMBuildStore(gen->builder, val, st->sret);
                LLVMSetAlignment(store, genlAggAlignOf(gen, valtype));
                LLVMBuildRetVoid(gen->builder);
                break;
            }
            case GenlRetVector: {
                unsigned n = 0;
                LLVMBuildRet(gen->builder, genlAggToVector(gen, val, LLVMGetPoison(coerced), &n));
                break;
            }
            case GenlRetInt:
                LLVMBuildRet(gen->builder, genlAggRecast(st, val, valtype, coerced, 1));
                break;
            default:
                return;
            }
            genlAggDead(st, inst);
            return;
        }
        if (!st->sret) {
            genlAggLoadOperands(st, inst);
            return;
        }
        genlAggCopyTo(st, st->sret, genlAggAlignOf(gen, LLVMTypeOf(val)), val);
        LLVMBuildRetVoid(gen->builder);
        genlAggDead(st, inst);
        return;
    }
    case LLVMPHI:
        // Its slot is made already, and filled from each predecessor last
        return;
    case LLVMSelect: {
        if (!large)
            break;
        GenlAggHome iftrue = *genlAggHomeOf(st, LLVMGetOperand(inst, 1));
        GenlAggHome iffalse = *genlAggHomeOf(st, LLVMGetOperand(inst, 2));
        LLVMValueRef home = LLVMBuildSelect(gen->builder, LLVMGetOperand(inst, 0), iftrue.home, iffalse.home, "aggsel");
        genlAggPut(&st->homes, inst, home, iftrue.align < iffalse.align ? iftrue.align : iffalse.align, 0);
        genlAggDead(st, inst);
        return;
    }
    case LLVMFreeze:
        if (!large)
            break;
        {
            GenlAggHome from = *genlAggHomeOf(st, LLVMGetOperand(inst, 0));
            genlAggPut(&st->homes, inst, from.home, from.align, 0);
        }
        genlAggDead(st, inst);
        return;
    default:
        break;
    }
    // Anything else keeps the value whole: it is loaded from its home for an
    // operand, and stored into a slot of its own when it makes one
    genlAggLoadOperands(st, inst);
    if (large)
        genlAggHomeOf(st, inst);
}

// ---- Phis -------------------------------------------------------------------

// The block an edge from 'pred' to 'blk' is copied in: 'pred' itself, unless
// it goes elsewhere too, where a block is put on the edge, and every phi of
// 'blk' made anew to name it
static LLVMBasicBlockRef genlAggEdge(GenlAggFn *st, LLVMBasicBlockRef pred, LLVMBasicBlockRef blk) {
    GenState *gen = st->gen;
    LLVMValueRef term = LLVMGetBasicBlockTerminator(pred);
    unsigned nsucc = LLVMGetNumSuccessors(term);
    if (nsucc <= 1)
        return pred;
    LLVMBasicBlockRef edge = LLVMAppendBasicBlockInContext(gen->context, st->fn, "aggedge");
    LLVMPositionBuilderAtEnd(gen->builder, edge);
    LLVMSetCurrentDebugLocation2(gen->builder, LLVMInstructionGetDebugLoc(term));
    LLVMBuildBr(gen->builder, blk);
    for (unsigned s = 0; s < nsucc; ++s) {
        if (LLVMGetSuccessor(term, s) == blk)
            LLVMSetSuccessor(term, s, edge);
    }
    // A phi names its predecessor, once for each edge, and there is one edge now
    LLVMValueRef phi = LLVMGetFirstInstruction(blk);
    while (phi && LLVMIsAPHINode(phi)) {
        LLVMValueRef next = LLVMGetNextInstruction(phi);
        LLVMPositionBuilderBefore(gen->builder, phi);
        LLVMValueRef newphi = LLVMBuildPhi(gen->builder, LLVMTypeOf(phi), "");
        unsigned nin = LLVMCountIncoming(phi);
        int named = 0;
        for (unsigned in = 0; in < nin; ++in) {
            LLVMBasicBlockRef from = LLVMGetIncomingBlock(phi, in);
            LLVMValueRef val = LLVMGetIncomingValue(phi, in);
            if (from == pred) {
                if (named)
                    continue;
                named = 1;
                from = edge;
            }
            LLVMAddIncoming(newphi, &val, &from, 1);
        }
        size_t len;
        const char *name = LLVMGetValueName2(phi, &len);
        LLVMSetValueName2(newphi, name, len);
        // A large phi's slot and its place in the list go to the new one
        for (uint32_t p = 0; p < st->nphis; ++p) {
            if (st->phis[p] == phi) {
                st->phis[p] = newphi;
                GenlAggHome home = *genlAggFind(&st->homes, phi);
                genlAggPut(&st->homes, newphi, home.home, home.align, home.owned);
            }
        }
        LLVMReplaceAllUsesWith(phi, newphi);
        LLVMInstructionEraseFromParent(phi);
        phi = next;
    }
    return edge;
}

// Each large phi's slot filled from each predecessor, at its end. Where a
// block has more than one, each is copied to a temporary first, then all
// into their slots, since one may take another's value of the last pass
static void genlAggPhis(GenlAggFn *st) {
    GenState *gen = st->gen;
    for (uint32_t p = 0; p < st->nphis; ++p) {
        LLVMValueRef phi = st->phis[p];
        if (!phi)
            continue;
        LLVMBasicBlockRef blk = LLVMGetInstructionParent(phi);
        // The block's large phis: this one and those after it in the list
        uint32_t nblk = 0;
        for (uint32_t q = p; q < st->nphis; ++q) {
            if (st->phis[q] && LLVMGetInstructionParent(st->phis[q]) == blk)
                ++nblk;
        }
        // Each predecessor, once
        unsigned nin = LLVMCountIncoming(phi);
        LLVMBasicBlockRef *preds = (LLVMBasicBlockRef *)malloc((nin + 1) * sizeof(LLVMBasicBlockRef));
        unsigned npreds = 0;
        for (unsigned in = 0; in < nin; ++in) {
            LLVMBasicBlockRef pred = LLVMGetIncomingBlock(phi, in);
            unsigned k;
            for (k = 0; k < npreds && preds[k] != pred; ++k)
                ;
            if (k == npreds)
                preds[npreds++] = pred;
        }
        for (unsigned k = 0; k < npreds; ++k) {
            LLVMBasicBlockRef pred = preds[k];
            LLVMBasicBlockRef at = genlAggEdge(st, pred, blk);
            // the edge may have made the block's phis anew
            LLVMValueRef term = LLVMGetBasicBlockTerminator(at);
            LLVMValueRef *temps = (LLVMValueRef *)malloc((nblk + 1) * sizeof(LLVMValueRef));
            uint32_t t = 0;
            for (uint32_t q = p; q < st->nphis; ++q) {
                LLVMValueRef bphi = st->phis[q];
                if (!bphi || LLVMGetInstructionParent(bphi) != blk)
                    continue;
                LLVMBasicBlockRef from = at == pred ? pred : at;
                LLVMValueRef val = NULL;
                unsigned bnin = LLVMCountIncoming(bphi);
                for (unsigned in = 0; in < bnin && !val; ++in) {
                    if (LLVMGetIncomingBlock(bphi, in) == from)
                        val = LLVMGetIncomingValue(bphi, in);
                }
                genlAggAt(st, term);
                GenlAggHome home = *genlAggFind(&st->homes, bphi);
                if (nblk > 1) {
                    temps[t] = genlAggSlot(st, LLVMTypeOf(bphi));
                    genlAggCopyTo(st, temps[t], home.align, val);
                    ++t;
                }
                else
                    genlAggCopyTo(st, home.home, home.align, val);
            }
            if (nblk > 1) {
                t = 0;
                for (uint32_t q = p; q < st->nphis; ++q) {
                    LLVMValueRef bphi = st->phis[q];
                    if (!bphi || LLVMGetInstructionParent(bphi) != blk)
                        continue;
                    GenlAggHome home = *genlAggFind(&st->homes, bphi);
                    LLVMBuildMemCpy(gen->builder, home.home, home.align, temps[t], home.align,
                        genlAggSize(gen, LLVMTypeOf(bphi)));
                    ++t;
                }
            }
            free(temps);
        }
        free(preds);
        // This block's large phis are done
        for (uint32_t q = p; q < st->nphis; ++q) {
            if (st->phis[q] && LLVMGetInstructionParent(st->phis[q]) == blk) {
                genlAggDead(st, st->phis[q]);
                st->phis[q] = NULL;
            }
        }
    }
}

// ---- Each function ------------------------------------------------------------

// The function's blocks, those reached from its entry first, in reverse
// postorder, so that a value is made before it is used except by a phi; then
// any nothing reaches
static LLVMBasicBlockRef *genlAggBlockOrder(LLVMValueRef fn, uint32_t *count) {
    uint32_t nblks = LLVMCountBasicBlocks(fn);
    LLVMBasicBlockRef *all = (LLVMBasicBlockRef *)malloc((nblks + 1) * sizeof(LLVMBasicBlockRef));
    LLVMGetBasicBlocks(fn, all);
    // Visited marks, by position in 'all'
    char *seen = (char *)calloc(nblks + 1, 1);
    LLVMBasicBlockRef *post = (LLVMBasicBlockRef *)malloc((nblks + 1) * sizeof(LLVMBasicBlockRef));
    uint32_t npost = 0;
    LLVMBasicBlockRef *stack = (LLVMBasicBlockRef *)malloc((nblks + 1) * sizeof(LLVMBasicBlockRef));
    unsigned *next = (unsigned *)malloc((nblks + 1) * sizeof(unsigned));
    uint32_t depth = 0;
#define GENLAGG_INDEX(b, out) { out = 0; while (all[out] != (b)) ++out; }
    stack[depth] = all[0];
    next[depth++] = 0;
    seen[0] = 1;
    while (depth > 0) {
        LLVMBasicBlockRef blk = stack[depth - 1];
        LLVMValueRef term = LLVMGetBasicBlockTerminator(blk);
        unsigned nsucc = term ? LLVMGetNumSuccessors(term) : 0;
        if (next[depth - 1] < nsucc) {
            LLVMBasicBlockRef succ = LLVMGetSuccessor(term, next[depth - 1]++);
            uint32_t i;
            GENLAGG_INDEX(succ, i);
            if (!seen[i]) {
                seen[i] = 1;
                stack[depth] = succ;
                next[depth++] = 0;
            }
        }
        else {
            post[npost++] = blk;
            --depth;
        }
    }
#undef GENLAGG_INDEX
    LLVMBasicBlockRef *order = (LLVMBasicBlockRef *)malloc((nblks + 1) * sizeof(LLVMBasicBlockRef));
    uint32_t n = 0;
    for (uint32_t i = npost; i-- > 0;)
        order[n++] = post[i];
    for (uint32_t i = 0; i < nblks; ++i) {
        if (!seen[i])
            order[n++] = all[i];
    }
    free(all); free(seen); free(post); free(stack); free(next);
    *count = n;
    return order;
}

// Does anything in the function make, take or return a large value, return
// its result otherwise than whole, or call a function that does?
static int genlAggFnLarge(GenState *gen, LLVMValueRef fn, GenlRetClass retclass, GenlAggHomes *params) {
    if (retclass != GenlRetWhole || params->used > 0)
        return 1;
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(blk); inst; inst = LLVMGetNextInstruction(inst)) {
            if (genlAggIsLarge(gen, LLVMTypeOf(inst)))
                return 1;
            if (LLVMIsAStoreInst(inst) && genlAggIsLarge(gen, LLVMTypeOf(LLVMGetOperand(inst, 0))))
                return 1;
            if (LLVMIsACallInst(inst) && genlAggSigChanged(gen, LLVMGetCalledFunctionType(inst)))
                return 1;
        }
    }
    return 0;
}

static void genlAggFn(GenState *gen, LLVMValueRef fn, GenlRetClass retclass, GenlAggHomes *params) {
    if (!genlAggFnLarge(gen, fn, retclass, params))
        return;
    GenlAggFn st;
    memset(&st, 0, sizeof(st));
    st.gen = gen;
    st.fn = fn;
    st.retclass = retclass;
    st.sret = genlAggRetSret(retclass) ? LLVMGetParam(fn, 0) : NULL;
    st.homes.mask = 63;
    st.homes.slots = (GenlAggHome *)calloc(64, sizeof(GenlAggHome));
    st.done.mask = 63;
    st.done.slots = (GenlAggHome *)calloc(64, sizeof(GenlAggHome));
    // The large parameters' homes, the pointers passed for them
    for (uint32_t i = 0; i <= params->mask; ++i) {
        GenlAggHome *p = &params->slots[i];
        if (p->key)
            genlAggPut(&st.homes, p->key, p->home, p->align, 0);
    }

    uint32_t nblks;
    LLVMBasicBlockRef *order = genlAggBlockOrder(fn, &nblks);
    // Each large phi's slot first, since a phi may take a value made after it
    LLVMValueRef entryfirst = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn));
    st.at = entryfirst;
    for (uint32_t b = 0; b < nblks; ++b) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(order[b]); inst && LLVMIsAPHINode(inst);
                inst = LLVMGetNextInstruction(inst)) {
            if (!genlAggIsLarge(gen, LLVMTypeOf(inst)))
                continue;
            genlAggAt(&st, entryfirst);
            LLVMValueRef slot = genlAggSlot(&st, LLVMTypeOf(inst));
            genlAggPut(&st.homes, inst, slot, genlAggAlignOf(gen, LLVMTypeOf(inst)), 1);
            genlAggPush(&st.phis, &st.nphis, &st.phimax, inst);
        }
    }
    for (uint32_t b = 0; b < nblks; ++b) {
        // This block's instructions as they were, since each is replaced as it goes
        uint32_t ninsts = 0, maxinsts = 0;
        LLVMValueRef *insts = NULL;
        for (LLVMValueRef inst = LLVMGetFirstInstruction(order[b]); inst; inst = LLVMGetNextInstruction(inst))
            genlAggPush(&insts, &ninsts, &maxinsts, inst);
        for (uint32_t i = 0; i < ninsts; ++i) {
            if (!genlAggIsDone(&st, insts[i]))
                genlAggInst(&st, insts[i]);
        }
        free(insts);
    }
    free(order);
    genlAggPhis(&st);

    // What was replaced goes, users first; a phi among them may still be
    // named by another, so each is first made unused
    for (uint32_t d = st.ndead; d-- > 0;) {
        LLVMValueRef inst = st.dead[d];
        if (LLVMGetTypeKind(LLVMTypeOf(inst)) != LLVMVoidTypeKind && LLVMGetFirstUse(inst))
            LLVMReplaceAllUsesWith(inst, LLVMGetPoison(LLVMTypeOf(inst)));
    }
    for (uint32_t d = st.ndead; d-- > 0;)
        LLVMInstructionEraseFromParent(st.dead[d]);
    free(st.dead);
    free(st.phis);
    free(st.done.slots);
    free(st.homes.slots);
}

// Move every aggregate value larger than GenlAggCopyMin bytes into memory,
// and return every smaller aggregate result in registers or a slot, over the
// whole module (see the head of this file)
void genlAggCopies(GenState *gen) {
    // The functions taking a large value or returning an aggregate, remade
    // with pointers or another result type; the old ones are deleted once
    // nothing uses their parameters
    uint32_t nold = 0, maxold = 0;
    LLVMValueRef *old = NULL;
    uint32_t nfns = 0, maxfns = 0;
    LLVMValueRef *fns = NULL;
    GenlAggHomes params;
    params.mask = 63;
    params.used = 0;
    params.slots = (GenlAggHome *)calloc(64, sizeof(GenlAggHome));
    GenlAggHomes consts;
    consts.mask = 63;
    consts.used = 0;
    consts.slots = (GenlAggHome *)calloc(64, sizeof(GenlAggHome));
    genlAggConsts = &consts;
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn))
        genlAggPush(&fns, &nfns, &maxfns, fn);
    // how each returns its result
    GenlRetClass *retclass = (GenlRetClass *)calloc(nfns + 1, sizeof(GenlRetClass));
    for (uint32_t f = 0; f < nfns; ++f) {
        LLVMValueRef fn = fns[f];
        if (LLVMGetIntrinsicID(fn) != 0 || genlAggIsCAbi(fn))
            continue;
        LLVMTypeRef fntype = LLVMGlobalGetValueType(fn);
        if (!genlAggSigChanged(gen, fntype))
            continue;
        retclass[f] = genlAggRetClassOf(gen, LLVMGetReturnType(fntype));
        fns[f] = genlAggRetype(gen, fn, &params);
        genlAggPush(&old, &nold, &maxold, fn);
    }

    // Each body
    for (uint32_t f = 0; f < nfns; ++f) {
        LLVMValueRef fn = fns[f];
        if (LLVMIsDeclaration(fn))
            continue;
        // Only this function's parameters are its homes
        GenlAggHomes own;
        own.mask = params.mask;
        own.used = 0;
        own.slots = (GenlAggHome *)calloc(params.mask + 1, sizeof(GenlAggHome));
        for (uint32_t i = 0; i <= params.mask; ++i) {
            GenlAggHome *p = &params.slots[i];
            if (p->key && LLVMGetParamParent(p->home) == fn)
                genlAggPut(&own, p->key, p->home, p->align, 0);
        }
        genlAggFn(gen, fn, retclass[f], &own);
        free(own.slots);
    }

    for (uint32_t o = 0; o < nold; ++o)
        LLVMDeleteFunction(old[o]);
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn))
        LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, GenlCAbiMarker, (unsigned)strlen(GenlCAbiMarker));
    free(retclass);
    free(params.slots);
    free(consts.slots);
    genlAggConsts = NULL;
    free(old);
    free(fns);
}
