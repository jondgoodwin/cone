/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../shared/timer.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
#include "../shared/fileio.h"
#include "genllvm.h"

#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Comdat.h>
#include <llvm-c/ErrorHandling.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef _WIN32
#define asmext "asm"
#define objext "obj"
#else
#define asmext "s"
#define objext "o"
#endif

// Generate parameter variable of the function 'fndcl' being generated
void genlParmVar(GenState *gen, FnDclNode *fndcl, VarDclNode *var) {
    assert(var->tag == VarDclTag);
    // We always alloca in case variable is mutable or we want to take address of its value
    var->llvmvar = genlAlloca(gen, genlType(gen, var->vtype), &var->namesym->namestr);
    genlRootNote(gen, var->llvmvar, var->vtype);
    LLVMBuildStore(gen->builder, genlFnDclParm(gen, fndcl, var), var->llvmvar);
    genlDropFlagBegin(gen, var, DropFlagWhole);
}

// Put a generated global in a COMDAT of its own, named for the symbol itself.
// A COFF section is otherwise all-or-nothing: without this, every function an
// object file defines ships in the executable, whether or not the program can
// reach it. With it, the linker discards the unreachable ones (/OPT:REF), and
// transitively their callees too. Only a definition may lead a COMDAT, so this
// belongs with generating the body, not with generating the name: an imported
// module's functions have bodies in the IR but are only declarations here.
//
// The selection kind says what the linker does when two object files both
// supply the symbol. Merging is what a generic instantiation needs and what
// nothing else wants: 'any' silently keeps one copy, which for every other
// symbol would turn a duplicate definition from a link error into a coin toss.
// So it follows the linkage the caller already chose.
void genlComdat(GenState *gen, LLVMValueRef global) {
    if (gen->comdats == ComdatNone)
        return;

    size_t namelen;
    const char *name = LLVMGetValueName2(global, &namelen);
    assert(namelen > 0 && "a COMDAT is named for its symbol, so the symbol needs a name");
    LLVMLinkage linkage = LLVMGetLinkage(global);
    int mergeable = linkage == LLVMLinkOnceAnyLinkage || linkage == LLVMLinkOnceODRLinkage
                 || linkage == LLVMWeakAnyLinkage || linkage == LLVMWeakODRLinkage;

    LLVMComdatRef comdat = LLVMGetOrInsertComdat(gen->module, name);
    LLVMSetComdatSelectionKind(comdat,
        (mergeable || gen->comdats == ComdatMergeOnly)?
            LLVMAnyComdatSelectionKind : LLVMNoDeduplicateComdatSelectionKind);
    LLVMSetComdat(global, comdat);
}

// An anonymous 'fn' literal is lifted to module scope with no name at all.
// Give it one that is private to this object file. It needs a name to lead a
// COMDAT, and it needs private linkage because nothing outside this object can
// refer to it: left public, the name LLVM's mangler invents is one that every
// other object file would invent too, and the two would collide.
static void genlNameAnonFn(GenState *gen, LLVMValueRef fn) {
    size_t namelen;
    LLVMGetValueName2(fn, &namelen);
    if (namelen > 0)
        return;
    LLVMSetValueName2(fn, "anon", 4);   // LLVM appends a suffix to keep it unique
    LLVMSetLinkage(fn, LLVMInternalLinkage);
}

// Whether a function is a 'main' returning nothing. The C runtime that calls
// 'main' takes its return as the process's exit status, so such a 'main' is
// generated returning i32, and each of its returns returns 0 (genlReturn).
int genlIsVoidMain(FnDclNode *fnnode, const char *symbol) {
    return strcmp(symbol, "main") == 0
        && itypeGetTypeDcl(((FnSigNode*)fnnode->vtype)->rettype)->tag == VoidTag;
}

// Generate a function
void genlFn(GenState *gen, FnDclNode *fnnode) {
    // Only a concrete declaration has an implementation. An overload name is a
    // namespace binding that selection replaces long before generation.
    assert(fnnode->tag == FnDclTag && "Only a concrete function/method is generated");
    if ((fnnode->flags & FlagInline) || fnnode->value->tag == IntrinsicTag)
        return;

    genlNameAnonFn(gen, fnnode->llvmvar);
    genlComdat(gen, fnnode->llvmvar);

    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    INode *svfnblock = gen->fnblock;
    FnDclNode *svfndcl = gen->fndcl;
    int svexitzero = gen->exitzero;
    GenRoots svroots;
    genlRootsSave(gen, &svroots);
    // A function generated while another is -- a drop asked for by a death --
    // finalizes only the temporaries it makes itself
    uint32_t svtempbase = gen->tempbase;
    gen->tempbase = gen->tempcnt;
    // A split method is generated here as its first half, which ends at each
    // seam (genlawait.c); its second halves after it
    Nodes *svseams = gen->seams;
    AwaitNode *svresumeat = gen->resumeat;
    uint32_t svflightbase = gen->flightbase;
    gen->seams = awaitSplitOf(fnnode);
    gen->resumeat = NULL;
    gen->flightbase = gen->flightcnt;

    FnSigNode *fnsig = (FnSigNode*)fnnode->vtype;
    assert(fnnode->value->tag == BlockTag);
    gen->fn = fnnode->llvmvar;
    gen->fnblock = fnnode->value;
    gen->fndcl = fnnode;
    size_t namelen;
    gen->exitzero = genlIsVoidMain(fnnode, LLVMGetValueName2(fnnode->llvmvar, &namelen));

    // Attach block and builder to function
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(gen->builder, entry);

    // Create our alloca insert point by generating a dummy instruction.
    // It will be erased after generating all LLVM IR code for the function
    LLVMValueRef allocaPoint = LLVMBuildAlloca(gen->builder, LLVMInt32TypeInContext(gen->context), "alloca_point");
    gen->allocaPoint = allocaPoint;

	// Generate LLVMValueRef's for all parameters, so we can use them as local vars in code
    uint32_t cnt;
    INode **nodesp;
    for (nodesFor(fnsig->parms, cnt, nodesp))
        genlParmVar(gen, fnnode, (VarDclNode*)*nodesp);

    // Generate the function's code (always a block). A drop the compiler gave
    // a type is built here, from the type's layout: an enum's block is empty,
    // and a struct's holds only its 'final' calls.
    if (structIsGeneratedDropFn((INode*)fnnode))
        genlTypeDrop(gen, fnnode);
    else
        genlBlock(gen, (BlockNode *)fnnode->value);

    // A function holding traced references links its frame of them
    genlRootFrame(gen);

	// erase temporary dummy alloca inserted earlier
    if (LLVMGetInstructionParent(allocaPoint))
        LLVMInstructionEraseFromParent(allocaPoint);

    LLVMDisposeBuilder(gen->builder);

    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fnblock = svfnblock;
    gen->fndcl = svfndcl;
    gen->exitzero = svexitzero;
    gen->tempcnt = gen->tempbase;
    gen->tempbase = svtempbase;
    genlRootsRestore(gen, &svroots);
    int split = gen->seams != NULL;
    gen->seams = svseams;
    gen->resumeat = svresumeat;
    gen->flightcnt = gen->flightbase;
    gen->flightbase = svflightbase;

    // A split method's second halves, one for each seam
    if (split)
        genlSplitHalves(gen, fnnode);

    // A compute entry point is a kernel on a GPU target, made beside it
    if (fnDclIsCompute(fnnode))
        genlComputeEntry(gen, fnnode);
}

// Insert every alloca before the allocaPoint in the function's entry block.
// Why? To improve LLVM optimization of SRoA and mem2reg, all allocas
// should be located in the function's entry block before the first call.
// Positioning before an instruction also takes that instruction's debug
// location, and the allocaPoint has none, so the builder's is put back after:
// without it, the next call in a function with debug info has no location,
// which the verifier refuses of a call that could be inlined.
LLVMValueRef genlAlloca(GenState *gen, LLVMTypeRef type, const char *name) {
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gen->builder);
    LLVMMetadataRef debugloc = LLVMGetCurrentDebugLocation2(gen->builder);
    LLVMPositionBuilderBefore(gen->builder, gen->allocaPoint);
    LLVMValueRef alloca = LLVMBuildAlloca(gen->builder, type, name);
    LLVMPositionBuilderAtEnd(gen->builder, current_block);
    LLVMSetCurrentDebugLocation2(gen->builder, debugloc);
    return alloca;
}

// ---- Roots: the shadow stack ------------------------------------------------
//
// Every function holding a traced reference on its stack links a frame of them
// into one chain, the head of which is conestd's 'cone_gcframes', so that a
// collector can find every live one (mem.traceRoots, conestd's
// 'cone_traceRoots'). A root is a stack slot whose type holds a traced
// reference: a local's or a parameter's (its alloca), or a birth's -- a slot
// of its own for each site that makes such a value outside any local, stored
// as soon as the value exists, so that no collection can find it only in a
// register. The frame is
//
//   { ptr prev, ptr map, [n x ptr] slot }
//
// each 'slot' the address of a root, and the map a private constant,
//
//   cone.roots.<k> = { usize n, [n x ptr] rec }
//
// each 'rec' the type record of the root at the same position, whose trace
// hands what the slot holds to 'mark'. A function with no root has no frame
// and generates exactly what it did before roots existed.

void genlRootNote(GenState *gen, LLVMValueRef slot, INode *vtype) {
    if (!itypeHoldsTraced(vtype))
        return;
    GenRoots *roots = &gen->roots;
    if (roots->cnt == roots->max) {
        uint32_t newmax = roots->max ? roots->max * 2 : 8;
        LLVMValueRef *slots = (LLVMValueRef *)memAllocBlk(newmax * sizeof(LLVMValueRef));
        INode **types = (INode **)memAllocBlk(newmax * sizeof(INode *));
        if (roots->cnt) {
            memcpy(slots, roots->slots, roots->cnt * sizeof(LLVMValueRef));
            memcpy(types, roots->types, roots->cnt * sizeof(INode *));
        }
        roots->slots = slots;
        roots->types = types;
        roots->max = newmax;
    }
    roots->slots[roots->cnt] = slot;
    roots->types[roots->cnt++] = vtype;
}

// One slot per site: a site run again (in a loop) overwrites what it stored
// last time, and what it stored stays rooted until then or the return
void genlRootBirth(GenState *gen, LLVMValueRef val, INode *vtype) {
    LLVMValueRef slot = genlAlloca(gen, genlType(gen, vtype), "birth");
    genlRootNote(gen, slot, vtype);
    LLVMBuildStore(gen->builder, val, slot);
}

void genlRootsSave(GenState *gen, GenRoots *saved) {
    *saved = gen->roots;
    memset(&gen->roots, 0, sizeof(GenRoots));
}

void genlRootsRestore(GenState *gen, GenRoots *saved) {
    gen->roots = *saved;
}

// The head of the chain of frames, defined by conestd; a declaration here
static LLVMValueRef genlRootChain(GenState *gen) {
    LLVMValueRef chain = LLVMGetNamedGlobal(gen->module, "cone_gcframes");
    if (chain == NULL)
        chain = LLVMAddGlobal(gen->module, LLVMPointerTypeInContext(gen->context, 0), "cone_gcframes");
    return chain;
}

// Build the frame of the function whose body was just generated, where it has
// any root. The push goes at the top of the entry block, before anything the
// body does: zero every root (a collection can come before a local is
// assigned), store each root's address and the map into the frame, link it at
// the head of the chain. The pop goes before every 'ret', found by scanning
// every block's terminator rather than trusting the return sites: a 'return'
// anywhere, from inside loops and nested blocks, and the function's end, all
// leave through one of them. 'break' and 'continue' stay in the function and
// need none. A panic aborts, so nothing unwinds past a pop.
void genlRootFrame(GenState *gen) {
    GenRoots *roots = &gen->roots;
    if (roots->cnt == 0)
        return;
    StructNode *recnode = typeRecordStruct();
    if (recnode == NULL)
        errorExit(ExitGen, "Internal error: a function holds traced references, and core declares no 'mem.typeRecord' whose TypeRecord its roots' map could hold");

    LLVMContextRef context = gen->context;
    LLVMTypeRef ptrtype = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef usize = genlUsize(gen);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    uint32_t n = roots->cnt;

    // The map: the count, then each root's type record, in slot order. A
    // record may generate its trace, a function of its own, which sets these
    // roots aside and back around itself.
    LLVMValueRef *recs = (LLVMValueRef *)memAllocBlk(n * sizeof(LLVMValueRef));
    for (uint32_t i = 0; i < n; ++i)
        recs[i] = genlTypeRecordOf(gen, roots->types[i], recnode);
    LLVMTypeRef maptypes[2] = { usize, LLVMArrayType2(ptrtype, n) };
    LLVMTypeRef maptype = LLVMStructTypeInContext(context, maptypes, 2, 0);
    char name[32];
    sprintf(name, "cone.roots.%u", gen->rootmaps++);
    LLVMValueRef map = LLVMAddGlobal(gen->module, maptype, name);
    LLVMSetGlobalConstant(map, 1);
    LLVMSetLinkage(map, LLVMPrivateLinkage);
    LLVMValueRef mapvals[2] = { LLVMConstInt(usize, n, 0), LLVMConstArray2(ptrtype, recs, n) };
    LLVMSetInitializer(map, LLVMConstStructInContext(context, mapvals, 2, 0));

    LLVMTypeRef frametypes[3] = { ptrtype, ptrtype, LLVMArrayType2(ptrtype, n) };
    LLVMTypeRef frametype = LLVMStructTypeInContext(context, frametypes, 3, 0);
    LLVMValueRef frame = genlAlloca(gen, frametype, "gcframe");
    LLVMValueRef chain = genlRootChain(gen);

    // The push, after the entry block's allocas and before its first
    // instruction that is not one
    LLVMValueRef first = LLVMGetNextInstruction(gen->allocaPoint);
    if (first)
        LLVMPositionBuilderBefore(gen->builder, first);
    else
        LLVMPositionBuilderAtEnd(gen->builder, LLVMGetInstructionParent(gen->allocaPoint));
    for (uint32_t i = 0; i < n; ++i)
        LLVMBuildStore(gen->builder, LLVMConstNull(genlType(gen, roots->types[i])), roots->slots[i]);
    for (uint32_t i = 0; i < n; ++i) {
        LLVMValueRef index[3] = { LLVMConstInt(i32, 0, 0), LLVMConstInt(i32, 2, 0), LLVMConstInt(i32, i, 0) };
        LLVMBuildStore(gen->builder, roots->slots[i], LLVMBuildInBoundsGEP2(gen->builder, frametype, frame, index, 3, "gcslot"));
    }
    LLVMBuildStore(gen->builder, map, LLVMBuildStructGEP2(gen->builder, frametype, frame, 1, "gcmap"));
    LLVMValueRef prev = LLVMBuildLoad2(gen->builder, ptrtype, chain, "gcprev");
    LLVMBuildStore(gen->builder, prev, LLVMBuildStructGEP2(gen->builder, frametype, frame, 0, "gcprevat"));
    LLVMBuildStore(gen->builder, frame, chain);

    // The pop, before every return: the previous frame is the head again
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(gen->fn); block; block = LLVMGetNextBasicBlock(block)) {
        LLVMValueRef term = LLVMGetBasicBlockTerminator(block);
        if (term == NULL || LLVMGetInstructionOpcode(term) != LLVMRet)
            continue;
        LLVMPositionBuilderBefore(gen->builder, term);
        LLVMBuildStore(gen->builder, prev, chain);
    }
}

// Whether a global's storage is a string literal's text plus a NUL. Such a
// global is not a copy of the literal: its storage is the initialized data,
// so like the literal's own it is one byte longer than its type, and its
// llvmvar is its address recast to a pointer to that type.
static int genlGloVarHasNul(VarDclNode *glovar) {
    return !(glovar->dclinfo.facts & DclExternal) && glovar->value && glovar->value->tag == StringLitTag;
}

// The LLVM global itself behind a global variable's llvmvar, which is a
// recast of it when the global carries a NUL. Under opaque pointers that
// recast folds away, and llvmvar is the global.
static LLVMValueRef genlGloVarGlobal(VarDclNode *glovar) {
    return LLVMIsAGlobalVariable(glovar->llvmvar) ? glovar->llvmvar : LLVMGetOperand(glovar->llvmvar, 0);
}

// Whether an immutable global's storage may be a constant, which LLVM may
// place in read-only memory and whose loads it may assume never change. Only
// what the object file initializes itself: a global without an initial value
// is assigned by its module's 'init', at run time, and an 'extern' one's value
// is another object's. Nor one whose type finalizes: the module's 'drop' hands
// it to that 'final' as '&uni', which may write it. Nor a thread-local, whose
// storage is each thread's own block, which no read-only section holds (the
// parser refuses one 'imm', and this keeps the rule where the flag is set).
// Nor one holding an atomic value, which atomic operations change even
// through 'imm' [Jon 26 Sep]: a write to read-only memory would fault, and
// every read of a constant could be folded to its initial value.
static int genlGloVarIsConstant(VarDclNode *glovar) {
    return permIsSame(glovar->perm, (INode*)immPerm) && glovar->value != NULL
        && !(glovar->dclinfo.facts & (DclExternal | DclThreadLocal))
        && itypeGetDropFnDcl(glovar->vtype) == NULL
        && !itypeHoldsAtomic(glovar->vtype);
}

// Generate global variable
void genlGloVar(GenState *gen, VarDclNode *varnode) {
    LLVMValueRef global = genlGloVarGlobal(varnode);

    // A GPU's logical addressing keeps no pointer in memory, so on a GPU target
    // no global holds a borrow (a string literal's global is its text)
    if (gen->opt->gpu && !genlGloVarHasNul(varnode) && itypeCarriesBorrow(varnode->vtype))
        errorMsgNode((INode*)varnode, ErrorGpuRefGlobal,
            "On a GPU target a global may not hold a reference: a pointer cannot be kept in GPU memory. Keep an index into what it would point at instead.");

    // An extern global is defined in some other object file; this one only
    // names it, and a declaration may not lead a COMDAT
    if (!(varnode->dclinfo.facts & DclExternal))
        genlComdat(gen, global);

    if (!varnode->value) {
        // If no value on non-extern, initialize with the zero initializer. A
        // GPU's workgroup memory has none: each workgroup's copy starts
        // undefined, an OpVariable with no initializer (one would need
        // Vulkan's shaderZeroInitializeWorkgroupMemory)
        if (!(varnode->dclinfo.facts & DclExternal))
            LLVMSetInitializer(global, gen->opt->gpu && (varnode->dclinfo.facts & DclWorkgroup)
                ? LLVMGetUndef(genlType(gen, varnode->vtype)) : LLVMConstNull(genlType(gen,varnode->vtype)));
        return;
    }

    // The text, and the NUL after it that the variable's type does not count
    else if (varnode->value->tag == StringLitTag) {
        SLitNode *strnode = (SLitNode*)varnode->value;
        LLVMSetInitializer(global, LLVMConstStringInContext2(gen->context, strnode->strlit, strnode->strlen, 0));
    }
    else
        LLVMSetInitializer(global, genlExpr(gen, varnode->value));

    // Mark initialized, immutable global variable as constant,
    // so it goes into a faster memory page that we know we will never be mutated
    if (genlGloVarIsConstant(varnode))
        LLVMSetGlobalConstant(global, 1);
}

// Whether this object file defines a declared symbol rather than merely
// declaring it: the declaration is not externally supplied, its module is one
// this compile generates bodies for, and a function has a body to generate.
// An imported module's functions have bodies in the IR and are declarations
// here, which is why the module's flag decides and not the node alone.
//
// A generic's instance is the exception: every object that uses one defines it,
// whether its generic's module is generated here or not. The generic's package
// cannot know which instances its importers make, so it has none for them to
// link against (genlImportedInstances generates the bodies).
static int genlIsDefinedHere(INode *dclnode) {
    DclInfo *dclinfo = inodeGetDclInfo(dclnode);
    if (dclinfo->facts & DclExternal)
        return 0;
    if (dclIsInstance(dclnode))
        return dclnode->tag != FnDclTag || ((FnDclNode*)dclnode)->value != NULL;
    ModuleNode *mod = dclInfoGetModule(dclnode);
    if (mod == NULL || !(mod->flags & FlagGenMod))
        return 0;
    return dclnode->tag != FnDclTag || ((FnDclNode*)dclnode)->value != NULL;
}

// What this object file does with a declared node's symbol.
//
// A generic's instance is defined in every object that uses it. In a described
// build that is several objects -- the generic's own package, and each package
// importing it -- so each copy is shared, and the linker keeps one. A compile
// with no build description is the program's only object, and its instances
// are its own, as its other definitions are.
static GenlDefinition genlDefinition(GenState *gen, INode *dclnode) {
    if (!genlIsDefinedHere(dclnode))
        return GenlDeclared;
    if (dclIsInstance(dclnode))
        return gen->opt->described ? GenlShared : GenlDefined;
    // The rule the include file's generator asks too, so the two agree (ir/export.c)
    return dclIsExported(gen->libroot, dclnode) ? GenlExported : GenlDefined;
}

// What this object does with a vtable it builds. Every object that coerces a
// type to a trait builds that pair's vtable, and a virtual reference carries its
// address to wherever it is passed: pattern matching tells the concrete type by
// comparing that address with its own object's vtable (genlIsType). So in a
// described build every copy is shared and the linker keeps one, as for an
// instance; alone, the program's vtables are its own.
GenlDefinition genlVtableDefinition(GenState *gen) {
    return gen->opt->described ? GenlShared : GenlDefined;
}

// Set a just-created global's linkage, storage class and calling convention
// together, from the declaring node's facts. The one place that decides them,
// so the COMDAT kind genlComdat later reads off the linkage cannot disagree
// with what was chosen here.
//
// The program rule: a definition is internal, since nothing outside this
// object may resolve against a program's symbols -- except 'main', which the C
// runtime resolves, and a public C-named one, which 'pub' and '@c' together
// export to C. A declaration is external, as an LLVM declaration can be nothing
// else. A system-convention ('@c(system)') function takes that convention
// whether it is defined here or not, and an 'extern' one is also imported from
// a DLL: the storage class is about reaching a symbol defined elsewhere, which
// a definition is not. Visibility is never set: a private name is a fact about
// the namespace, not the object file.
//
// The library rule adds one case: a definition the library exports to its
// importers (dclIsExported) keeps the external linkage LLVM gave it, and so
// also survives optimisation when nothing in the library itself uses it.
//
// A described build adds the shared case: a generic's instance, or a vtable,
// that every object using it defines is 'linkonce_odr', and genlComdat reads
// that as a COMDAT of kind 'any', so the linker keeps one of the identical
// copies and drops the rest. With 'nodeduplicate' the copies would be a
// duplicate-symbol error (LNK2005); internal, each object would keep its own,
// and two objects' vtables for one type would have two addresses.
//
// 'dclnode' is NULL for a vtable, which no node declares. 'defined' says what
// this object does with the symbol.
void genlLinkage(LLVMValueRef global, INode *dclnode, GenlDefinition defined) {
    if (defined == GenlShared) {
        LLVMSetLinkage(global, LLVMLinkOnceODRLinkage);
        return;
    }
    if (dclnode) {
        DclInfo *dclinfo = inodeGetDclInfo(dclnode);
        if (dclnode->tag == FnDclTag && (dclinfo->facts & DclSystemCC)) {
            LLVMSetFunctionCallConv(global, LLVMX86StdcallCallConv);
            if (dclinfo->facts & DclExternal)
                LLVMSetDLLStorageClass(global, LLVMDLLImportStorageClass);
        }
        if ((dclinfo->facts & DclCName) && !(dclinfo->facts & DclPrivate))
            return;
    }
    if (defined != GenlDefined)
        return;
    size_t namelen;
    const char *name = LLVMGetValueName2(global, &namelen);
    if (namelen == 4 && memcmp(name, "main", 4) == 0)
        return;
    LLVMSetLinkage(global, LLVMInternalLinkage);
}

// ---- One symbol, one global [Jon 25 Sep] ----------------------------------
//
// LLVM keeps one global per name: a second function or global added under a
// name the module already holds is renamed ('abs.1'), which nothing defines,
// and the link fails. Two declarations may spell one symbol legitimately -- two
// modules that each declare C's 'abs' -- so each global a declaration adds is
// checked for that rename, and genlClaimSymbol decides who has the symbol.

// Whether a global is local to this object: an internal or private symbol is
// reached through its LLVM value alone, and nothing links against its name
static int genlIsLocal(LLVMValueRef global) {
    LLVMLinkage linkage = LLVMGetLinkage(global);
    return linkage == LLVMInternalLinkage || linkage == LLVMPrivateLinkage;
}

// The LLVM global a declaring node's symbol is, beneath the recast through
// which a global carrying a NUL is reached
static LLVMValueRef genlSymGlobal(INode *node) {
    return node->tag == FnDclTag ? ((FnDclNode*)node)->llvmvar : genlGloVarGlobal((VarDclNode*)node);
}

// The declaration that added a global, or NULL for one the compiler made
// itself: a vtable, a thunk, a string literal, C's 'free', an intrinsic
static INode *genlSymOwner(GenState *gen, LLVMValueRef global) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(gen->symnodes, cnt, nodesp)) {
        if (genlSymGlobal(*nodesp) == global)
            return *nodesp;
    }
    return NULL;
}

// Whether two declarations of one symbol declare the same thing, as the object
// file sees it: two functions of one LLVM function type and one calling
// convention, or two globals of one LLVM value type, one permission, and both
// thread-local or neither
static int genlSymAgree(GenState *gen, INode *a, INode *b) {
    if (a->tag != b->tag)
        return 0;
    if (a->tag == FnDclTag)
        return genlFnDclType(gen, (FnDclNode*)a) == genlFnDclType(gen, (FnDclNode*)b)
            && (inodeGetDclInfo(a)->facts & DclSystemCC) == (inodeGetDclInfo(b)->facts & DclSystemCC);
    VarDclNode *va = (VarDclNode*)a;
    VarDclNode *vb = (VarDclNode*)b;
    return genlType(gen, va->vtype) == genlType(gen, vb->vtype) && permIsSame(va->perm, vb->perm)
        && (va->dclinfo.facts & DclThreadLocal) == (vb->dclinfo.facts & DclThreadLocal);
}

static void genlSymSetVar(INode *node, LLVMValueRef var) {
    if (node->tag == FnDclTag)
        ((FnDclNode*)node)->llvmvar = var;
    else
        ((VarDclNode*)node)->llvmvar = var;
}

static void genlSymDelete(LLVMValueRef global) {
    if (LLVMIsAFunction(global))
        LLVMDeleteFunction(global);
    else
        LLVMDeleteGlobal(global);
}

// A declaration's global has just been added, linkage and all, and was to be
// named 'symbol'. Where LLVM had to rename it, another global already has that
// name, and one of them gives way:
// - two symbols neither of which is C-named are two of Cone's own spellings
//   meeting, which is a compiler defect;
// - a local global takes no particular name, so a local newcomer keeps the name
//   LLVM gave it, and a local holder of the name gives it up;
// - two declarations the linker sees must agree (genlSymAgree), or it is
//   ErrorCNameConflict. Then a declaration shares the global already there,
//   and a definition takes it over from the declarations before it, so the
//   definition owns the symbol whichever is reached first. Two definitions are
//   ErrorCNameDefTwice.
// An error leaves the renamed global in place, so generation can carry on;
// genpgm then emits nothing.
static void genlClaimSymbol(GenState *gen, INode *node, LLVMValueRef global, GenlDefinition defined, char *symbol) {
    size_t namelen;
    const char *name = LLVMGetValueName2(global, &namelen);
    size_t symlen = strlen(symbol);
    if (namelen == symlen && memcmp(name, symbol, symlen) == 0) {
        nodesAdd(&gen->symnodes, node);
        return;
    }

    LLVMValueRef existing = LLVMGetNamedFunction(gen->module, symbol);
    if (existing == NULL)
        existing = LLVMGetNamedGlobal(gen->module, symbol);
    INode *owner = existing ? genlSymOwner(gen, existing) : NULL;
    if (existing == NULL || (owner && !(inodeGetDclInfo(node)->facts & DclCName)
        && !(inodeGetDclInfo(owner)->facts & DclCName)))
        errorUnreachable(node, "two declarations spelled one symbol, and neither has a C name");

    if (genlIsLocal(global)) {
        nodesAdd(&gen->symnodes, node);
        return;
    }
    if (genlIsLocal(existing)) {
        LLVMSetValueName2(existing, "", 0);
        LLVMSetValueName2(global, symbol, symlen);
        LLVMSetValueName2(existing, symbol, symlen);  // LLVM appends a suffix
        if (LLVMGetComdat(existing))
            genlComdat(gen, existing);   // a COMDAT is named for its symbol
        nodesAdd(&gen->symnodes, node);
        return;
    }

    // An external symbol the compiler declared itself, which has no declaring
    // node: an LLVM intrinsic or a C runtime entry it calls by name ('llvm.trap'
    // and 'cone_panicIndex', genlPanic). A declaration meeting one shares it
    // where it can
    if (owner == NULL) {
        if (node->tag == FnDclTag && defined == GenlDeclared
            && LLVMIsAFunction(existing) && LLVMIsDeclaration(existing)) {
            LLVMTypeRef fnptr = LLVMTypeOf(global);
            genlSymSetVar(node, LLVMTypeOf(existing) == fnptr ? existing : LLVMConstBitCast(existing, fnptr));
            LLVMDeleteFunction(global);
            nodesAdd(&gen->symnodes, node);
        }
        else
            errorMsgNode(node, ErrorCNameConflict,
                "The C name %s is a symbol the compiler generates itself. Give this declaration another C name.",
                symbol);
        return;
    }

    if (!genlSymAgree(gen, owner, node)) {
        errorMsgNode(node, ErrorCNameConflict,
            "The C name %s is declared differently at %s:%u. Every declaration of one C name must agree: a function in its signature, a global in its type, its permission and whether it is '@threadlocal'.",
            symbol, owner->lexer->url, owner->linenbr);
        return;
    }
    int ownerdefines = genlDefinition(gen, owner) != GenlDeclared;
    if (ownerdefines && defined != GenlDeclared) {
        errorMsgNode(node, ErrorCNameDefTwice,
            "The C name %s is defined already, at %s:%u. One C name has one definition: declare it without a body everywhere else.",
            symbol, owner->lexer->url, owner->linenbr);
        return;
    }
    if (defined == GenlDeclared) {
        genlSymSetVar(node, node->tag == FnDclTag ? ((FnDclNode*)owner)->llvmvar : ((VarDclNode*)owner)->llvmvar);
        genlSymDelete(global);
    }
    else {
        // No body refers to the declaration yet, but a vtable built while
        // typing a signature may
        LLVMValueRef var = node->tag == FnDclTag ? ((FnDclNode*)node)->llvmvar : ((VarDclNode*)node)->llvmvar;
        LLVMReplaceAllUsesWith(existing, var);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(gen->symnodes, cnt, nodesp)) {
            if (genlSymGlobal(*nodesp) == existing)
                genlSymSetVar(*nodesp, var);
        }
        genlSymDelete(existing);
        LLVMSetValueName2(global, symbol, symlen);
    }
    nodesAdd(&gen->symnodes, node);
}

// Generate LLVMValueRef for a global variable
// It sets appropriate visibility, linkage and constant flags for the linker
void genlGloVarName(GenState *gen, VarDclNode *glovar) {
    // Named once: an instance's static may be reached both by the symbol pass
    // and by genlImportedInstances
    if (glovar->llvmvar)
        return;
    char symbol[2048];
    LLVMTypeRef vartype = genlType(gen, glovar->vtype);
    LLVMValueRef global;
    // A string literal's text is stored with a NUL after it, for C, as the
    // literal's own global is. Every Cone use goes through a pointer to the
    // variable's type, so the NUL is not counted and no Cone store reaches it.
    if (genlGloVarHasNul(glovar)) {
        SLitNode *strnode = (SLitNode*)glovar->value;
        LLVMTypeRef nultype = LLVMArrayType(LLVMInt8TypeInContext(gen->context), strnode->strlen + 1);
        global = LLVMAddGlobal(gen->module, nultype, nameSymbol(symbol, (INode*)glovar));
        // On a GPU target the global is in an address space of its own, and
        // its uses take the flat address (genlFlatAddr)
        glovar->llvmvar = gen->opt->gpu ? global : LLVMConstBitCast(global, LLVMPointerType(vartype, 0));
    }
    // On a GPU target a '@workgroup' global is in the Workgroup storage class
    // (genlgpusync.c); everywhere else it is an ordinary global
    else if (gen->opt->gpu && (glovar->dclinfo.facts & DclWorkgroup))
        global = glovar->llvmvar = LLVMAddGlobalInAddressSpace(gen->module, vartype,
            nameSymbol(symbol, (INode*)glovar), genlGpuGlobalSpace(glovar));
    else
        global = glovar->llvmvar = LLVMAddGlobal(gen->module, vartype, nameSymbol(symbol, (INode*)glovar));

    // A thread-local's storage is each thread's own copy, its declarations
    // elsewhere marked alike so every object reaches it the same way. The model
    // is general-dynamic, LLVM's default, which is right whether the global ends
    // up in the program or in a shared library: a linker building an executable
    // relaxes it to the cheaper models itself, and Windows has one TLS access
    // sequence (through the TEB and '_tls_index') whatever the model says.
    if (glovar->dclinfo.facts & DclThreadLocal)
        LLVMSetThreadLocal(global, 1);

    // Mark immutable global variables as 'constant', so they can appear in immutable blocks
    // This improves performance
    if (genlGloVarIsConstant(glovar))
        LLVMSetGlobalConstant(global, 1);

    GenlDefinition defined = genlDefinition(gen, (INode*)glovar);
    genlLinkage(global, (INode*)glovar, defined);
    genlClaimSymbol(gen, (INode*)glovar, global, defined, symbol);
}

// Generate LLVMValueRef for a global function
void genlGloFnName(GenState *gen, FnDclNode *glofn) {
    // Do not generate inline functions
    if (glofn->flags & FlagInline)
        return;

    // A candidate is reached both by its own namespace entry and through the
    // overload set it joins, so only generate its symbol the first time
    if (glofn->llvmvar)
        return;

    // Add function to the module
    if (glofn->value == NULL || glofn->value->tag != IntrinsicTag) {
        // Typing the signature can declare this very function. A method that
        // fills a slot of a trait and takes a virtual reference to that trait
        // builds the trait's vtable here, and the vtable asks for the symbol of
        // every method in its slots, this one included. Declaring it a second
        // time would leave that first declaration bodiless in the vtable.
        LLVMTypeRef fntype = genlFnDclType(gen, glofn);
        if (glofn->llvmvar)
            return;
        char symbol[2048];
        nameSymbol(symbol, (INode*)glofn);
        if (genlIsVoidMain(glofn, symbol)) {
            unsigned parmcnt = LLVMCountParamTypes(fntype);
            LLVMTypeRef *parmtypes = memAllocBlk((parmcnt ? parmcnt : 1) * sizeof(LLVMTypeRef));
            LLVMGetParamTypes(fntype, parmtypes);
            fntype = LLVMFunctionType(LLVMInt32TypeInContext(gen->context), parmtypes, parmcnt, LLVMIsFunctionVarArg(fntype));
        }
        glofn->llvmvar = LLVMAddFunction(gen->module, symbol, fntype);
        genlCAbiDeclare(gen, glofn, glofn->llvmvar);
        GenlDefinition defined = genlDefinition(gen, (INode*)glofn);
        genlLinkage(glofn->llvmvar, (INode*)glofn, defined);
        genlClaimSymbol(gen, (INode*)glofn, glofn->llvmvar, defined, symbol);
        // A function returning 'Never' does not return, which lets LLVM take
        // every path into a call to it as cold and end the path there
        if (glofn->vtype->tag == FnSigTag && itypeIsNever(((FnSigNode*)glofn->vtype)->rettype)
            && LLVMIsAFunction(glofn->llvmvar))
            genlFnAttr(gen, glofn->llvmvar, "noreturn");

        // Add metadata on implemented functions (debug mode only). Implemented
        // HERE: an imported module's function has a body in the IR and is only a
        // declaration in this object, and LLVM's verifier rejects a declaration
        // carrying a subprogram
        if (!gen->opt->release && defined != GenlDeclared) {
            char *fnname = glofn->namesym? &glofn->namesym->namestr : "";
            LLVMMetadataRef fntype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder,
                gen->difile, NULL, 0, 0);
            LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
                fnname, strlen(fnname), symbol, strlen(symbol),
                gen->difile, glofn->linenbr, fntype, 0, 1, glofn->linenbr, LLVMDIFlagPublic, 0);
            LLVMSetSubprogram(glofn->llvmvar, sp);
        }
    }
}

void genlGlobalSyms(GenState *gen, INode *node);

// Generate the global symbols for one instance of a generic type. Each method
// is owned by the instance, so its symbol reads as the instance then the
// method, and it is defined by whichever module owns the generic. An instance
// of a generic trait or enum splits its members as a non-generic one does: its
// methods belong to the implementers, its static functions to itself, and
// genlGlobalImpl generates their bodies.
static void genlGenericInstanceSyms(GenState *gen, INode *instance) {
    if (instance->tag != StructTag)
        return;
    int istrait = instance->flags & TraitType;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((INsTypeNode*)instance)->nodelist, cnt, nodesp)) {
        if (istrait && ((*nodesp)->flags & FlagMethFld))
            continue;
        genlGlobalSyms(gen, *nodesp);
    }
}

// Generate module or type global symbols
void genlGlobalSyms(GenState *gen, INode *node) {
    // Handle type nodes
    if (isTypeNode(node)) {
        // An extension's copies of its base's variants are reachable only
        // through it, as a generic's instances are through memonodes. A generic
        // extension's copies are templates, and this reaches their instances.
        if (node->tag == StructTag) {
            uint32_t copies = structEnumCopyCount((StructNode*)node);
            uint32_t pos;
            for (pos = 0; pos < copies; ++pos)
                genlGlobalSyms(gen, nodesGet(((StructNode*)node)->derived, pos));
        }
        // A generic type is a symbol only through its instantiations, exactly as
        // a generic function is: its own methods are uncloned templates with a
        // type parameter for a receiver, and nothing can be generated for them.
        // The instances live in memonodes and are reachable no other way, so
        // without this a method of a generic struct got no symbol at all and the
        // call to it loaded a null function.
        if (node->tag == StructTag && ((StructNode*)node)->genericinfo) {
            Nodes *memonodes = ((StructNode*)node)->genericinfo->memonodes;
            if (memonodes == NULL)
                return;
            uint32_t cnt;
            INode **nodesp;
            for (nodesFor(memonodes, cnt, nodesp)) {
                ++nodesp; --cnt;  // memonodes holds pairs: the call, then what it instantiated
                genlGenericInstanceSyms(gen, *nodesp);
            }
            return;
        }
        // For types with a namespace, let's do its nodes too
        if (isMethodType(node)) {
            INsTypeNode *tnode = (INsTypeNode*)node;
            INode **nodesp;
            uint32_t cnt;
            int istrait = node->tag == StructTag && (node->flags & TraitType);
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
                // A trait's methods are not its own to generate: a requirement has
                // no body, and a default was cloned into each implementer, which
                // owns and names the copy. Its static functions are its own --
                // nothing inherits or dispatches them -- so they are generated
                // here or nowhere, and 'Trait.name()' loaded a null without this.
                if (istrait && ((*nodesp)->flags & FlagMethFld))
                    continue;
                genlGlobalSyms(gen, *nodesp);
            }
        }
        return;
    }

    switch (node->tag) {
    case VarDclTag:
        genlGloVarName(gen, (VarDclNode *)node);
        break;
    case FnDclTag:
        if (((FnDclNode*)node)->genericinfo) {
            // A generic is only ever a symbol through its instantiations, and a
            // generic this compilation unit never called has none: memonodes is
            // still the null it was created as.
            Nodes *memonodes = ((FnDclNode*)node)->genericinfo->memonodes;
            if (memonodes == NULL)
                break;
            uint32_t cnt;
            INode **nodesp;
            for (nodesFor(memonodes, cnt, nodesp)) {
                ++nodesp; --cnt;
                genlGloFnName(gen, (FnDclNode *)*nodesp);
            }
        }
        else
            genlGloFnName(gen, (FnDclNode *)node);
        break;
    // An overload name has no symbol of its own. Each of its candidates is also
    // a node of the module or type that owns it, and is named there: a public
    // name holds only public candidates (fnOverloadDclAdd), and a private name
    // is filtered along with its private candidates.
    case FnOverloadDclTag:
    // A macro expands where it is used and leaves no symbol behind
    case MacroDclTag:
    // An alias is a binding and not a declaration, so it emits nothing: what it
    // stands for is named where that is declared
    case AliasDclTag:
        break;
    }
}

// Generate module or type implementation (e.g., function blocks)
void genlGlobalImpl(GenState *gen, INode *node) {
    // Handle type nodes
    if (isTypeNode(node)) {
        // As in genlGlobalSyms: an extension's copies are reached through it
        if (node->tag == StructTag) {
            uint32_t copies = structEnumCopyCount((StructNode*)node);
            uint32_t pos;
            for (pos = 0; pos < copies; ++pos)
                genlGlobalImpl(gen, nodesGet(((StructNode*)node)->derived, pos));
        }
        // As in genlGlobalSyms: a generic type has bodies to generate only in
        // its instances, which are reachable through memonodes alone
        if (node->tag == StructTag && ((StructNode*)node)->genericinfo) {
            Nodes *memonodes = ((StructNode*)node)->genericinfo->memonodes;
            if (memonodes == NULL)
                return;
            uint32_t cnt;
            INode **nodesp;
            for (nodesFor(memonodes, cnt, nodesp)) {
                ++nodesp; --cnt;  // memonodes holds pairs: the call, then what it instantiated
                genlGlobalImpl(gen, *nodesp);
            }
            return;
        }
        // For types with a namespace, let's do its nodes too. Same split as
        // genlGlobalSyms: a trait's methods belong to its implementers, its
        // static functions to itself.
        if (isMethodType(node)) {
            INsTypeNode *tnode = (INsTypeNode*)node;
            INode **nodesp;
            uint32_t cnt;
            int istrait = node->tag == StructTag && (node->flags & TraitType);
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
                if (istrait && ((*nodesp)->flags & FlagMethFld))
                    continue;
                genlGlobalImpl(gen, *nodesp);
            }
        }
        return;
    }

    switch (node->tag) {
    case VarDclTag:
        genlGloVar(gen, (VarDclNode*)node);
        break;

    case FnDclTag:
        if (((FnDclNode*)node)->genericinfo) {
            // As above: an uninstantiated generic has no memonodes to generate
            Nodes *memonodes = ((FnDclNode*)node)->genericinfo->memonodes;
            if (memonodes == NULL)
                break;
            uint32_t cnt;
            INode **nodesp;
            for (nodesFor(memonodes, cnt, nodesp)) {
                ++nodesp; --cnt;
                genlFn(gen, (FnDclNode*)*nodesp);
            }
        }
        else if (((FnDclNode*)node)->value) {
            genlFn(gen, (FnDclNode*)node);
        }
        break;

    case ImportTag:
    case FieldDclTag:
    case MacroDclTag:
    case ConstDclTag:
    // An overload name has no implementation of its own to generate
    case FnOverloadDclTag:
    // Nor has an alias: it is a binding, and what it stands for is generated
    // wherever that is declared
    case AliasDclTag:
    // Nor has a module trait: a requirement has no body, and each default is
    // generated as the copy each conforming module owns (modTraitConform)
    case ModTraitTag:
        break;

    default:
        errorUnreachable(node, "a module-level declaration code generation has no case for");
        break;
    }
}

// Define every instance of a generic that a module this object does not
// generate has: an imported package's generic, instantiated by this compile.
// The package cannot know which instances its importers make, so it has none
// for them to link against, and the importer defines each one it uses from the
// body its include file carries (genlIsDefinedHere). A private generic counts
// too -- a public generic's body may instantiate it -- and the symbol pass skips
// an imported module's private names, so an instance is named here if nothing
// named it yet. Walks a module's node as genlGlobalImpl does, but generates
// only instances.
static void genlImportedInstances(GenState *gen, INode *node) {
    if (isTypeNode(node)) {
        if (node->tag != StructTag)
            return;
        StructNode *strnode = (StructNode*)node;
        uint32_t copies = structEnumCopyCount(strnode);
        uint32_t pos;
        for (pos = 0; pos < copies; ++pos)
            genlImportedInstances(gen, nodesGet(strnode->derived, pos));
        INode **nodesp;
        uint32_t cnt;
        if (strnode->genericinfo) {
            Nodes *memonodes = strnode->genericinfo->memonodes;
            if (memonodes == NULL)
                return;
            for (nodesFor(memonodes, cnt, nodesp)) {
                ++nodesp; --cnt;  // memonodes holds pairs: the call, then what it instantiated
                genlGenericInstanceSyms(gen, *nodesp);
                genlGlobalImpl(gen, *nodesp);
            }
            return;
        }
        // A non-generic type's generic methods
        int istrait = node->flags & TraitType;
        for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
            if (istrait && ((*nodesp)->flags & FlagMethFld))
                continue;
            genlImportedInstances(gen, *nodesp);
        }
        return;
    }
    if (node->tag == FnDclTag && ((FnDclNode*)node)->genericinfo) {
        Nodes *memonodes = ((FnDclNode*)node)->genericinfo->memonodes;
        if (memonodes == NULL)
            return;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(memonodes, cnt, nodesp)) {
            ++nodesp; --cnt;
            genlGloFnName(gen, (FnDclNode*)*nodesp);
            genlFn(gen, (FnDclNode*)*nodesp);
        }
    }
}

// The program's stitched init and final [Jon 23 Sep]. Each module declares only
// its own portion, and this object's view of the program is stitched into two
// functions: one calling every module's 'init' in the module order, each module
// after everything it depends on and the root last, and one calling every
// module's finalizer in exactly the reverse, the root first. A module with
// neither gets no call. The order is pgm->initorder (pgmModuleOrder), and a
// module this object does not generate -- another package, reached through its
// include file -- is called by its symbol, which its own object exports
// (dclIsExported). The functions are this object's own, internal, and made
// only when a call asks for one: 'initAll()' and 'finalAll()' today, the entry
// glue once it is built.
LLVMValueRef genlStitchFn(GenState *gen, int16_t intrinsic) {
    int which = intrinsic == InitAllIntrinsic ? 0 : 1;
    if (gen->stitch[which] == NULL) {
        LLVMTypeRef fntype = LLVMFunctionType(LLVMVoidTypeInContext(gen->context), NULL, 0, 0);
        gen->stitch[which] = LLVMAddFunction(gen->module, which == 0 ? "cone.initAll" : "cone.finalAll", fntype);
        genlLinkage(gen->stitch[which], NULL, GenlDefined);
    }
    return gen->stitch[which];
}

static void genlStitch(GenState *gen, int which) {
    LLVMValueRef fn = gen->stitch[which];
    if (fn == NULL)
        return;
    genlComdat(gen, fn);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(gen->context, fn, "entry"));
    Nodes *order = gen->pgm->initorder;
    uint32_t count = order->used;
    uint32_t pos;
    for (pos = 0; pos < count; ++pos) {
        ModuleNode *mod = (ModuleNode*)nodesGet(order, which == 0 ? pos : count - 1 - pos);
        FnDclNode *lifefn = which == 0 ? mod->initfn : mod->finalfn;
        if (lifefn == NULL)
            continue;
        if (lifefn->llvmvar == NULL)
            genlGloFnName(gen, lifefn);
        LLVMBuildCall2(builder, genlType(gen, lifefn->vtype), lifefn->llvmvar, NULL, 0, "");
    }
    LLVMBuildRetVoid(builder);
    LLVMDisposeBuilder(builder);
}

// Generate the program
void genlProgram(GenState *gen, ProgramNode *pgm) {

    assert(pgm->tag == ProgramTag);
    gen->module = LLVMModuleCreateWithNameInContext(gen->opt->srcname, gen->context);
    // The target comes before any IR: the builder and the passes fold sizes,
    // offsets and alignments against the module's layout, and a module without
    // one gets LLVM's default, where i64 aligns to 4
    LLVMSetTarget(gen->module, gen->opt->triple);
    char *layout = LLVMCopyStringRepOfTargetData(gen->datalayout);
    LLVMSetDataLayout(gen->module, layout);
    LLVMDisposeMessage(layout);
    if (!gen->opt->release) {
        gen->dibuilder = LLVMCreateDIBuilder(gen->module);
        gen->difile = LLVMDIBuilderCreateFile(gen->dibuilder, gen->opt->srcpath, strlen(gen->opt->srcpath), ".", 1);
        // The compile unit is attached to the module; nothing reads it back
        LLVMDIBuilderCreateCompileUnit(gen->dibuilder, LLVMDWARFSourceLanguageC,
            gen->difile, "Cone compiler", 13, 0, "", 0, 0, "", 0, LLVMDWARFEmissionFull, 0, 0, 0, "", 0, "", 0);
    }

    // A library exports what its root and submodules define (dclIsExported).
    // The root is the program's first module, added before core (parsePgm).
    gen->libroot = gen->opt->library ? (ModuleNode*)nodesGet(pgm->modules, 0) : NULL;
    gen->pgm = pgm;
    gen->stitch[0] = gen->stitch[1] = NULL;
    gen->symnodes = newNodes(64);
    gen->tyrectypes = NULL;
    gen->tyrecs = NULL;
    gen->tyreccnt = gen->tyrecmax = 0;
    gen->tyrecnothing = NULL;
    gen->tyrecuntraced = NULL;
    memset(&gen->roots, 0, sizeof(GenRoots));
    gen->rootmaps = 0;
    gen->entries = NULL;
    gen->entrycnt = gen->entrymax = 0;
    gen->gpusites = NULL;

    // First, generate global symbols for all modules, so that forward references succeed
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        ModuleNode *mod = (ModuleNode*)*nodesp;
        int16_t generating = mod->flags & FlagGenMod;
        // A generic module is compiled only as its instances: each is a module
        // of the program (pgmTypeCheck), generated in every object that uses it
        if (mod->genericinfo)
            continue;

        uint32_t icnt;
        INode **inodesp;
        for (nodesFor(mod->nodes, icnt, inodesp)) {
            // generate node's global name only if not a private name in a non-generating module
            if (generating || !inodeIsPrivate(*inodesp))
                genlGlobalSyms(gen, *inodesp);
        }
    }

    // Now generate implementation logic, including function logic or var init
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        ModuleNode *mod = (ModuleNode*)*nodesp;
        if (mod->genericinfo)
            continue;

        // Generate implementation only for module(s) flagged for generation,
        // and of any other module, only the instances this compile made of its
        // generics
        uint32_t icnt;
        INode **inodesp;
        for (nodesFor(mod->nodes, icnt, inodesp)) {
            if (mod->flags & FlagGenMod)
                genlGlobalImpl(gen, *inodesp);
            else
                genlImportedInstances(gen, *inodesp);
        }
    }

    // Last, the stitched init and final a call asked for, once every module's
    // lifecycle functions have their symbols
    genlStitch(gen, 0);
    genlStitch(gen, 1);

    if (!gen->opt->release)
        LLVMDIBuilderFinalize(gen->dibuilder);
}

// Use provided options (triple, etc.) to creation a machine
LLVMTargetMachineRef genlCreateMachine(ConeOptions *opt) {
    char *err;
    LLVMTargetRef target;
    LLVMCodeGenOptLevel opt_level;
    LLVMRelocMode reloc;
    LLVMTargetMachineRef machine;

    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllTargets();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();

    // Find target for the specified triple
    if (!opt->triple)
        opt->triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(opt->triple, &target, &err) != 0) {
        errorMsg(ErrorGenErr, "Could not create target: %s", err);
        LLVMDisposeMessage(err);
        return NULL;
    }

    // Create a specific target machine
    opt_level = opt->release? LLVMCodeGenLevelAggressive : LLVMCodeGenLevelNone;
    reloc = (opt->pic || opt->library)? LLVMRelocPIC : LLVMRelocDefault;
    // The CPU is the portable baseline unless named. 'native', which LLVM's
    // target machine does not know, is this machine's CPU and every feature it
    // has, as clang's -march=native means
    if (!opt->cpu)
        opt->cpu = "generic";
    else if (strcmp(opt->cpu, "native") == 0) {
        opt->cpu = LLVMGetHostCPUName();
        if (!opt->features)
            opt->features = LLVMGetHostCPUFeatures();
    }
    if (!opt->features)
        opt->features = "";
    if (!(machine = LLVMCreateTargetMachine(target, opt->triple, opt->cpu, opt->features, opt_level, reloc, LLVMCodeModelDefault))) {
        errorMsg(ErrorGenErr, "Could not create target machine");
        return NULL;
    }

    return machine;
}

// Generate requested object file
void genlOut(char *objpath, char *asmpath, LLVMModuleRef mod, LLVMTargetMachineRef machine) {
    char *err;

    // Generate assembly file if requested. LLVM's SPIR-V backend rewrites the
    // module it emits (its intrinsics, its own types), and crashes emitting
    // that again, so on a GPU target the assembly is emitted from a copy.
    if (asmpath) {
        int gpu = strncmp(LLVMGetTarget(mod), "spirv", 5) == 0;
        LLVMModuleRef asmmod = gpu ? LLVMCloneModule(mod) : mod;
        if (LLVMTargetMachineEmitToFile(machine, asmmod, asmpath, LLVMAssemblyFile, &err) != 0) {
            errorMsg(ErrorGenErr, "Could not emit asm file: %s", err);
            LLVMDisposeMessage(err);
        }
        if (gpu)
            LLVMDisposeModule(asmmod);
    }

    // Generate .o or .obj file
    if (LLVMTargetMachineEmitToFile(machine, mod, objpath, LLVMObjectFile, &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
        LLVMDisposeMessage(err);
    }
}

// *********************
// GPU targets: every function inlined, and none calling itself
// *********************

// The functions this object defines, in an open-addressed table by their LLVM
// value, each with its state in the walk of who calls whom
typedef struct {
    LLVMValueRef *fns;
    uint8_t *state;         // 0 not yet walked, 1 on the walk's path, 2 walked
    uint32_t mask;
    LLVMValueRef *path;     // the walk's path, outermost caller first
    uint32_t depth;
} GenlCallWalk;

static uint32_t genlCallSlot(GenlCallWalk *walk, LLVMValueRef fn) {
    uint32_t i = (uint32_t)(((uintptr_t)fn >> 4) * 2654435761u) & walk->mask;
    while (walk->fns[i] && walk->fns[i] != fn)
        i = (i + 1) & walk->mask;
    return i;
}

// A function's name and line, for the message: its declaration's where the
// compiler has one, else its symbol
static void genlCallName(GenState *gen, LLVMValueRef fn, char *buf, size_t size) {
    INode *owner = genlSymOwner(gen, fn);
    size_t len;
    if (owner && owner->tag == FnDclTag)
        snprintf(buf, size, "'%s' (line %u)", &((FnDclNode*)owner)->namesym->namestr, owner->linenbr);
    else
        snprintf(buf, size, "'%s'", LLVMGetValueName2(fn, &len));
}

// Report the cycle on the walk's path from 'head', which the function last on
// the path calls
static void genlRecursion(GenState *gen, GenlCallWalk *walk, LLVMValueRef head) {
    char chain[1024];
    char name[256];
    size_t used = 0;
    uint32_t from = 0;
    while (walk->path[from] != head)
        ++from;
    chain[0] = '\0';
    for (uint32_t i = from; i < walk->depth && used < sizeof(chain) - 1; ++i) {
        genlCallName(gen, walk->path[i], name, sizeof(name));
        used += snprintf(chain + used, sizeof(chain) - used, "%s%s", i == from ? "" : i == from + 1 ? " calls " : ", which calls ", name);
    }
    if (used < sizeof(chain) - 1)
        snprintf(chain + used, sizeof(chain) - used, walk->depth - from > 1 ? ", which calls it again" : " calls itself");
    INode *owner = genlSymOwner(gen, head);
    const char *msg = "On a GPU target a function may not call itself, directly or through others: every function is inlined there, and Vulkan's SPIR-V allows no recursion. %s.";
    if (owner)
        errorMsgNode(owner, ErrorGpuRecursion, msg, chain);
    else
        errorMsg(ErrorGpuRecursion, msg, chain);
}

static void genlCallWalk(GenState *gen, GenlCallWalk *walk, LLVMValueRef fn) {
    walk->state[genlCallSlot(walk, fn)] = 1;
    walk->path[walk->depth++] = fn;
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(blk); inst; inst = LLVMGetNextInstruction(inst)) {
            if (!LLVMIsACallInst(inst))
                continue;
            LLVMValueRef callee = LLVMGetCalledValue(inst);
            if (!LLVMIsAFunction(callee) || LLVMIsDeclaration(callee))
                continue;
            uint8_t *state = &walk->state[genlCallSlot(walk, callee)];
            if (*state == 1)
                genlRecursion(gen, walk, callee);
            else if (*state == 0)
                genlCallWalk(gen, walk, callee);
        }
    }
    walk->state[genlCallSlot(walk, fn)] = 2;
    --walk->depth;
}

// A GPU has no call stack to speak of, and Vulkan's SPIR-V types a pointer by
// the memory it points into: only with every call inlined does each borrow
// have one origin, whose address space LLVM's inference then settles. So
// every function this object defines is marked 'alwaysinline', which the
// always-inliner honours at every optimization level (genpgm), and one that
// calls itself, which no inliner can remove, is refused.
static void genlGpuCalls(GenState *gen) {
    uint32_t nfns = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
        if (!LLVMIsDeclaration(fn))
            ++nfns;
    }
    GenlCallWalk walk;
    uint32_t cap = 16;
    while (cap < nfns * 2)
        cap <<= 1;
    walk.mask = cap - 1;
    walk.fns = (LLVMValueRef *)memAllocBlk(cap * sizeof(LLVMValueRef));
    walk.state = (uint8_t *)memAllocBlk(cap);
    walk.path = (LLVMValueRef *)memAllocBlk((nfns + 1) * sizeof(LLVMValueRef));
    walk.depth = 0;
    memset(walk.fns, 0, cap * sizeof(LLVMValueRef));
    memset(walk.state, 0, cap);
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
        if (LLVMIsDeclaration(fn))
            continue;
        walk.fns[genlCallSlot(&walk, fn)] = fn;
        // A kernel is what everything is inlined into (genlgpu.c)
        int kernel = 0;
        for (uint32_t e = 0; e < gen->entrycnt; ++e)
            kernel = kernel || gen->entries[e].kernel == fn;
        if (!kernel)
            genlFnAttr(gen, fn, "alwaysinline");
    }
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
        if (!LLVMIsDeclaration(fn) && walk.state[genlCallSlot(&walk, fn)] == 0)
            genlCallWalk(gen, &walk, fn);
    }
}

// LLVM 23's SPIR-V backend mishandles a struct or array held as one value (a
// first-class aggregate) wherever more than one instruction carries it: a phi
// of one crashes its pointer-cast legalisation when an incoming value is a
// parameter; its structurizer breaks dominance when an incoming value is a
// constant, or when the value is made in a loop and used after it; and it
// breaks a function returning a struct taken whole out of another. So on a
// GPU target, after optimization, every aggregate is carried as its scalar
// leaves: a phi or select of one becomes one per leaf, inserting and
// extracting parts only renames leaves, and a use that takes the value whole
// (a return, a store, a call) gets it rebuilt from its leaves just before it.
// What makes an aggregate (a parameter, a load, a call) has its leaves
// extracted just after it. One of more leaves than this is left whole.
#define GenlAggMaxLeaves 256

typedef struct {
    LLVMValueRef key;       // an aggregate value
    LLVMValueRef *leaves;   // its scalar leaves, in field order
} GenlAggSlot;

// Each aggregate's leaves, in an open-addressed table by its LLVM value
typedef struct {
    GenlAggSlot *slots;
    uint32_t mask;
    uint32_t used;
} GenlAggMap;

static int genlIsAggType(LLVMTypeRef type) {
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    return kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind;
}

// The number of scalar leaves of a type, nested aggregates flattened
static uint32_t genlAggLeafCount(LLVMTypeRef type) {
    switch (LLVMGetTypeKind(type)) {
    case LLVMStructTypeKind: {
        uint32_t n = 0;
        unsigned cnt = LLVMCountStructElementTypes(type);
        for (unsigned i = 0; i < cnt && n <= GenlAggMaxLeaves; ++i)
            n += genlAggLeafCount(LLVMStructGetTypeAtIndex(type, i));
        return n;
    }
    case LLVMArrayTypeKind: {
        uint64_t len = LLVMGetArrayLength2(type);
        uint32_t each = len ? genlAggLeafCount(LLVMGetElementType(type)) : 0;
        if (each == 0)
            return 0;
        return len > GenlAggMaxLeaves || len * each > GenlAggMaxLeaves ? GenlAggMaxLeaves + 1 : (uint32_t)(len * each);
    }
    default:
        return 1;
    }
}

static uint32_t genlAggElemCount(LLVMTypeRef type) {
    return LLVMGetTypeKind(type) == LLVMStructTypeKind
        ? LLVMCountStructElementTypes(type) : (uint32_t)LLVMGetArrayLength2(type);
}

static LLVMTypeRef genlAggElemType(LLVMTypeRef type, uint32_t i) {
    return LLVMGetTypeKind(type) == LLVMStructTypeKind
        ? LLVMStructGetTypeAtIndex(type, i) : LLVMGetElementType(type);
}

// The leaves of a value, extracted at the builder's position, or of a
// constant, folded
static void genlAggSplit(GenState *gen, LLVMValueRef val, LLVMValueRef **leaves) {
    LLVMTypeRef type = LLVMTypeOf(val);
    if (!genlIsAggType(type)) {
        *(*leaves)++ = val;
        return;
    }
    // A part with no leaves, an empty struct or a zero-length array, is
    // neither taken out nor put back: the backend cannot select a value of
    // no size
    uint32_t cnt = genlAggElemCount(type);
    for (uint32_t i = 0; i < cnt; ++i) {
        if (genlAggLeafCount(genlAggElemType(type, i)) == 0)
            continue;
        LLVMValueRef elem = LLVMIsAConstant(val)
            ? LLVMGetAggregateElement(val, i)
            : LLVMBuildExtractValue(gen->builder, val, i, "");
        genlAggSplit(gen, elem, leaves);
    }
}

// An aggregate of a type built from its leaves at the builder's position
static LLVMValueRef genlAggBuild(GenState *gen, LLVMTypeRef type, LLVMValueRef **leaves) {
    if (!genlIsAggType(type))
        return *(*leaves)++;
    LLVMValueRef agg = LLVMGetPoison(type);
    uint32_t cnt = genlAggElemCount(type);
    for (uint32_t i = 0; i < cnt; ++i) {
        if (genlAggLeafCount(genlAggElemType(type, i)) > 0)
            agg = LLVMBuildInsertValue(gen->builder, agg, genlAggBuild(gen, genlAggElemType(type, i), leaves), i, "");
    }
    return agg;
}

// Whether a value is an aggregate this carries as leaves
static int genlAggCarried(LLVMValueRef val) {
    LLVMTypeRef type = LLVMTypeOf(val);
    return genlIsAggType(type) && genlAggLeafCount(type) <= GenlAggMaxLeaves;
}

static GenlAggSlot *genlAggSlot(GenlAggMap *map, LLVMValueRef key) {
    uint32_t i = (uint32_t)(((uintptr_t)key >> 4) * 2654435761u) & map->mask;
    while (map->slots[i].key && map->slots[i].key != key)
        i = (i + 1) & map->mask;
    return &map->slots[i];
}

// Room for a value's leaves, which it is then filled in
static LLVMValueRef *genlAggPut(GenlAggMap *map, LLVMValueRef key) {
    if ((map->used + 1) * 2 > map->mask + 1) {
        GenlAggSlot *old = map->slots;
        uint32_t oldcap = map->mask + 1;
        map->mask = oldcap * 2 - 1;
        map->slots = (GenlAggSlot *)calloc(oldcap * 2, sizeof(GenlAggSlot));
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (old[i].key)
                *genlAggSlot(map, old[i].key) = old[i];
        }
        free(old);
    }
    uint32_t nleaves = genlAggLeafCount(LLVMTypeOf(key));
    GenlAggSlot *slot = genlAggSlot(map, key);
    slot->key = key;
    slot->leaves = (LLVMValueRef *)malloc((nleaves ? nleaves : 1) * sizeof(LLVMValueRef));
    ++map->used;
    return slot->leaves;
}

// The first leaf of the part of an aggregate type an index path names, and
// that part's type
static uint32_t genlAggPathLeaf(LLVMTypeRef type, const unsigned *idx, unsigned nidx, LLVMTypeRef *part) {
    uint32_t first = 0;
    for (unsigned d = 0; d < nidx; ++d) {
        for (unsigned i = 0; i < idx[d]; ++i)
            first += genlAggLeafCount(genlAggElemType(type, i));
        type = genlAggElemType(type, idx[d]);
    }
    *part = type;
    return first;
}

static LLVMValueRef *genlAggLeaves(GenState *gen, GenlAggMap *map, LLVMValueRef fn, LLVMValueRef val);

// A scalar as a leaf: a scalar taken out of a carried aggregate is that
// aggregate's leaf
static LLVMValueRef genlAggScalar(GenState *gen, GenlAggMap *map, LLVMValueRef fn, LLVMValueRef val) {
    if (!LLVMIsAExtractValueInst(val) || !genlAggCarried(LLVMGetOperand(val, 0)))
        return val;
    LLVMValueRef agg = LLVMGetOperand(val, 0);
    LLVMTypeRef part;
    uint32_t first = genlAggPathLeaf(LLVMTypeOf(agg), LLVMGetIndices(val), LLVMGetNumIndices(val), &part);
    return genlAggLeaves(gen, map, fn, agg)[first];
}

// The leaves of a carried aggregate, found once
static LLVMValueRef *genlAggLeaves(GenState *gen, GenlAggMap *map, LLVMValueRef fn, LLVMValueRef val) {
    GenlAggSlot *slot = genlAggSlot(map, val);
    if (slot->key)
        return slot->leaves;
    LLVMTypeRef type = LLVMTypeOf(val);
    uint32_t nleaves = genlAggLeafCount(type);
    LLVMValueRef *leaves;

    // Putting a part in renames the part's leaves
    if (LLVMIsAInsertValueInst(val)) {
        LLVMValueRef *from = genlAggLeaves(gen, map, fn, LLVMGetOperand(val, 0));
        LLVMTypeRef part;
        uint32_t first = genlAggPathLeaf(type, LLVMGetIndices(val), LLVMGetNumIndices(val), &part);
        LLVMValueRef elem = LLVMGetOperand(val, 1);
        LLVMValueRef scalar = NULL;
        LLVMValueRef *partleaves = genlIsAggType(part)
            ? genlAggLeaves(gen, map, fn, elem)
            : (scalar = genlAggScalar(gen, map, fn, elem), &scalar);
        leaves = genlAggPut(map, val);
        memcpy(leaves, from, nleaves * sizeof(LLVMValueRef));
        memcpy(leaves + first, partleaves, genlAggLeafCount(part) * sizeof(LLVMValueRef));
        return leaves;
    }

    // Taking a part out of a carried aggregate is a range of its leaves
    if (LLVMIsAExtractValueInst(val) && genlAggCarried(LLVMGetOperand(val, 0))) {
        LLVMValueRef agg = LLVMGetOperand(val, 0);
        LLVMValueRef *from = genlAggLeaves(gen, map, fn, agg);
        LLVMTypeRef part;
        uint32_t first = genlAggPathLeaf(LLVMTypeOf(agg), LLVMGetIndices(val), LLVMGetNumIndices(val), &part);
        leaves = genlAggPut(map, val);
        memcpy(leaves, from + first, nleaves * sizeof(LLVMValueRef));
        return leaves;
    }

    // A select of aggregates is a select per leaf
    if (LLVMIsASelectInst(val)) {
        LLVMValueRef *iftrue = genlAggLeaves(gen, map, fn, LLVMGetOperand(val, 1));
        LLVMValueRef *iffalse = genlAggLeaves(gen, map, fn, LLVMGetOperand(val, 2));
        leaves = genlAggPut(map, val);
        LLVMPositionBuilderBefore(gen->builder, val);
        for (uint32_t i = 0; i < nleaves; ++i)
            leaves[i] = LLVMBuildSelect(gen->builder, LLVMGetOperand(val, 0), iftrue[i], iffalse[i], "");
        return leaves;
    }

    // A constant's are folded; what else makes an aggregate (a parameter, a
    // load, a call) has its leaves extracted just after it
    leaves = genlAggPut(map, val);
    if (LLVMIsAArgument(val)) {
        // After the entry block's allocas, which stay first
        LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
        LLVMValueRef first = LLVMGetFirstInstruction(entry);
        while (LLVMIsAAllocaInst(first))
            first = LLVMGetNextInstruction(first);
        LLVMPositionBuilder(gen->builder, entry, first);
    }
    else if (!LLVMIsAConstant(val))
        LLVMPositionBuilder(gen->builder, LLVMGetInstructionParent(val), LLVMGetNextInstruction(val));
    LLVMValueRef *next = leaves;
    genlAggSplit(gen, val, &next);
    return leaves;
}

static int genlAggRenames(LLVMValueRef inst) {
    return (LLVMIsAInsertValueInst(inst) || LLVMIsAExtractValueInst(inst)
        || LLVMIsAPHINode(inst) || LLVMIsASelectInst(inst)) && genlAggCarried(inst);
}

static void genlGpuAggregatesFn(GenState *gen, GenlAggMap *map, LLVMValueRef fn) {
    // The carried aggregates: parameters and instructions
    uint32_t navail = 64, nvals = 0;
    LLVMValueRef *vals = (LLVMValueRef *)malloc(navail * sizeof(LLVMValueRef));
    unsigned nparams = LLVMCountParams(fn);
    for (unsigned i = 0; i < nparams; ++i) {
        if (nvals == navail)
            vals = (LLVMValueRef *)realloc(vals, (navail *= 2) * sizeof(LLVMValueRef));
        if (genlAggCarried(LLVMGetParam(fn, i)))
            vals[nvals++] = LLVMGetParam(fn, i);
    }
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(blk); inst; inst = LLVMGetNextInstruction(inst)) {
            if (nvals == navail)
                vals = (LLVMValueRef *)realloc(vals, (navail *= 2) * sizeof(LLVMValueRef));
            if (genlAggCarried(inst))
                vals[nvals++] = inst;
        }
    }
    if (nvals == 0) {
        free(vals);
        return;
    }

    // Each aggregate phi becomes a phi per leaf, all made before any is
    // filled, since a loop's phis take each other
    for (uint32_t v = 0; v < nvals; ++v) {
        LLVMValueRef phi = vals[v];
        if (!LLVMIsAPHINode(phi))
            continue;
        uint32_t nleaves = genlAggLeafCount(LLVMTypeOf(phi));
        LLVMValueRef *typed = (LLVMValueRef *)malloc((nleaves ? nleaves : 1) * sizeof(LLVMValueRef));
        LLVMValueRef *next = typed;
        genlAggSplit(gen, LLVMGetPoison(LLVMTypeOf(phi)), &next);
        LLVMValueRef *leaves = genlAggPut(map, phi);
        LLVMPositionBuilderBefore(gen->builder, phi);
        for (uint32_t i = 0; i < nleaves; ++i)
            leaves[i] = LLVMBuildPhi(gen->builder, LLVMTypeOf(typed[i]), "");
        free(typed);
    }
    for (uint32_t v = 0; v < nvals; ++v) {
        LLVMValueRef phi = vals[v];
        if (!LLVMIsAPHINode(phi))
            continue;
        uint32_t nleaves = genlAggLeafCount(LLVMTypeOf(phi));
        LLVMValueRef *leaves = genlAggLeaves(gen, map, fn, phi);
        unsigned nin = LLVMCountIncoming(phi);
        for (unsigned in = 0; in < nin; ++in) {
            LLVMBasicBlockRef inblk = LLVMGetIncomingBlock(phi, in);
            LLVMValueRef *src = genlAggLeaves(gen, map, fn, LLVMGetIncomingValue(phi, in));
            for (uint32_t i = 0; i < nleaves; ++i)
                LLVMAddIncoming(leaves[i], &src[i], &inblk, 1);
        }
    }

    // Every use of a carried aggregate: a part taken out is its leaf, what
    // renames leaves needs nothing, and anything else gets it rebuilt
    uint32_t ndead = 0, deadavail = 64;
    LLVMValueRef *dead = (LLVMValueRef *)malloc(deadavail * sizeof(LLVMValueRef));
    for (uint32_t v = 0; v < nvals; ++v) {
        LLVMValueRef val = vals[v];
        uint32_t nusers = 0;
        for (LLVMUseRef use = LLVMGetFirstUse(val); use; use = LLVMGetNextUse(use))
            ++nusers;
        LLVMValueRef *users = (LLVMValueRef *)malloc((nusers ? nusers : 1) * sizeof(LLVMValueRef));
        nusers = 0;
        for (LLVMUseRef use = LLVMGetFirstUse(val); use; use = LLVMGetNextUse(use))
            users[nusers++] = LLVMGetUser(use);
        for (uint32_t u = 0; u < nusers; ++u) {
            LLVMValueRef user = users[u];
            if (!LLVMIsAInstruction(user) || genlAggRenames(user))
                continue;
            if (LLVMIsAExtractValueInst(user) && !genlIsAggType(LLVMTypeOf(user))) {
                LLVMValueRef leaf = genlAggScalar(gen, map, fn, user);
                if (leaf == user)       // one extracted here, as a leaf
                    continue;
                LLVMReplaceAllUsesWith(user, leaf);
                if (ndead == deadavail)
                    dead = (LLVMValueRef *)realloc(dead, (deadavail *= 2) * sizeof(LLVMValueRef));
                dead[ndead++] = user;
                continue;
            }
            if (LLVMIsAPHINode(user))   // an aggregate phi too large to carry
                continue;
            LLVMValueRef *next = genlAggLeaves(gen, map, fn, val);
            LLVMPositionBuilderBefore(gen->builder, user);
            LLVMValueRef whole = genlAggBuild(gen, LLVMTypeOf(val), &next);
            int nops = LLVMGetNumOperands(user);
            for (int op = 0; op < nops; ++op) {
                if (LLVMGetOperand(user, op) == val)
                    LLVMSetOperand(user, op, whole);
            }
        }
        free(users);
    }

    // What carried the aggregates whole is now unused: the scalars taken out,
    // then the renamings, latest first, once the phis among them, which may
    // take each other, take nothing
    for (uint32_t d = 0; d < ndead; ++d)
        LLVMInstructionEraseFromParent(dead[d]);
    for (uint32_t v = 0; v < nvals; ++v) {
        LLVMValueRef phi = vals[v];
        if (!LLVMIsAPHINode(phi))
            continue;
        unsigned nin = LLVMCountIncoming(phi);
        for (unsigned in = 0; in < nin; ++in)
            LLVMSetOperand(phi, in, LLVMGetPoison(LLVMTypeOf(phi)));
    }
    int erased;
    do {
        erased = 0;
        for (uint32_t v = nvals; v-- > 0;) {
            LLVMValueRef val = vals[v];
            if (val && !LLVMIsAArgument(val) && genlAggRenames(val) && !LLVMGetFirstUse(val)) {
                LLVMInstructionEraseFromParent(val);
                vals[v] = NULL;
                erased = 1;
            }
        }
    } while (erased);
    free(dead);
    free(vals);
}

// How many first fields down from 'outer' 'inner' is (0 when they are the
// same type), or -1 when it is not there: a pointer to the one is a pointer
// to the other, but not to SPIR-V
static int genlFirstFieldDepth(LLVMTypeRef outer, LLVMTypeRef inner) {
    int depth = 0;
    while (outer != inner) {
        LLVMTypeKind kind = LLVMGetTypeKind(outer);
        if (kind == LLVMStructTypeKind && LLVMCountStructElementTypes(outer) > 0)
            outer = LLVMStructGetTypeAtIndex(outer, 0);
        else if (kind == LLVMArrayTypeKind)
            outer = LLVMGetElementType(outer);
        else
            return -1;
        ++depth;
    }
    return depth;
}

// The type an address computation reaches
static LLVMTypeRef genlGepResultType(LLVMValueRef gep) {
    LLVMTypeRef type = LLVMGetGEPSourceElementType(gep);
    int nops = LLVMGetNumOperands(gep);
    for (int op = 2; op < nops; ++op) {
        if (LLVMGetTypeKind(type) == LLVMStructTypeKind)
            type = LLVMStructGetTypeAtIndex(type, (unsigned)LLVMConstIntGetZExtValue(LLVMGetOperand(gep, op)));
        else
            type = LLVMGetElementType(type);
    }
    return type;
}

// The type a pointer parameter points to, as its uses agree on it: the one
// every type it is used as is a first field of, or NULL
static LLVMTypeRef genlParamPointee(LLVMValueRef param) {
    LLVMTypeRef pointee = NULL;
    for (LLVMUseRef use = LLVMGetFirstUse(param); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        LLVMTypeRef type;
        if (LLVMIsAGetElementPtrInst(user) && LLVMGetOperand(user, 0) == param)
            type = LLVMGetGEPSourceElementType(user);
        else if (LLVMIsALoadInst(user))
            type = LLVMTypeOf(user);
        else if (LLVMIsAStoreInst(user) && LLVMGetOperand(user, 1) == param)
            type = LLVMTypeOf(LLVMGetOperand(user, 0));
        else
            continue;
        if (pointee == NULL || genlFirstFieldDepth(type, pointee) >= 0)
            pointee = type;
        else if (genlFirstFieldDepth(pointee, type) < 0)
            return NULL;
    }
    return pointee;
}

// The type a pointer is known to point to, or NULL
LLVMTypeRef genlGpuPointee(LLVMValueRef ptr) {
    if (LLVMIsAAllocaInst(ptr))
        return LLVMGetAllocatedType(ptr);
    if (LLVMIsAGetElementPtrInst(ptr))
        return genlGepResultType(ptr);
    if (LLVMIsAGlobalVariable(ptr))
        return LLVMGlobalGetValueType(ptr);
    if (LLVMIsAArgument(ptr))
        return genlParamPointee(ptr);
    // A storage buffer's element (genlgpu.c), its run-time array's element type
    LLVMValueRef callee = LLVMIsACallInst(ptr) ? LLVMGetCalledValue(ptr) : NULL;
    if (callee && LLVMIsAFunction(callee)
        && LLVMGetIntrinsicID(callee) == LLVMLookupIntrinsicID("llvm.spv.resource.getpointer", 28)) {
        LLVMTypeRef contents = LLVMGetTargetExtTypeTypeParam(LLVMTypeOf(LLVMGetOperand(ptr, 0)), 0);
        return LLVMGetTypeKind(contents) == LLVMArrayTypeKind ? LLVMGetElementType(contents) : NULL;
    }
    return NULL;
}

// LLVM 23's SPIR-V backend types a pointer by the address computations made
// from it, and with opaque pointers LLVM drops one whose indices are all
// zero (instsimplify, GVN and the inliner each fold it): a struct's first
// field is then read through the struct's own pointer, and an array field
// indexed from it, so the backend indexes the struct itself (by a run-time
// index, or past a scalar) and the module is refused. So on a GPU target,
// after optimization, a load, store or address computation using a pointer
// as a type it holds as a first field gets the zero indices put back.
static void genlGpuRetypeFn(GenState *gen, LLVMValueRef fn) {
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        LLVMValueRef inst = LLVMGetFirstInstruction(blk);
        while (inst) {
            LLVMValueRef next = LLVMGetNextInstruction(inst);
            int ptrop;
            LLVMTypeRef used;
            if (LLVMIsALoadInst(inst)) {
                ptrop = 0;
                used = LLVMTypeOf(inst);
            }
            else if (LLVMIsAStoreInst(inst)) {
                ptrop = 1;
                used = LLVMTypeOf(LLVMGetOperand(inst, 0));
            }
            else if (LLVMIsAGetElementPtrInst(inst)) {
                ptrop = 0;
                used = LLVMGetGEPSourceElementType(inst);
            }
            else if (LLVMIsAAtomicRMWInst(inst) || LLVMIsAAtomicCmpXchgInst(inst)) {
                // An Atomic[T]'s value is its first field
                ptrop = 0;
                used = LLVMTypeOf(LLVMGetOperand(inst, 1));
            }
            else {
                inst = next;
                continue;
            }
            LLVMValueRef ptr = LLVMGetOperand(inst, ptrop);
            LLVMTypeRef pointee = genlGpuPointee(ptr);
            int depth = pointee ? genlFirstFieldDepth(pointee, used) : -1;
            // The other way about: GVN takes two addresses that are one
            // number for one value, so an array field's address can be its
            // first element's, then indexed as the array; the first
            // element's address, its trailing zero indices dropped, is the
            // array's
            int outer = depth < 0 && pointee && LLVMIsAGetElementPtrInst(ptr) ? genlFirstFieldDepth(used, pointee) : -1;
            if (outer > 0) {
                int nbase = LLVMGetNumOperands(ptr);
                int zeros = 0;
                while (zeros < outer && nbase - 1 - zeros >= 2) {
                    LLVMValueRef op = LLVMGetOperand(ptr, nbase - 1 - zeros);
                    if (!LLVMIsAConstantInt(op) || LLVMConstIntGetZExtValue(op) != 0)
                        break;
                    ++zeros;
                }
                if (zeros == outer && nbase <= 64) {
                    LLVMValueRef idx[64];
                    int n = 0;
                    for (int op = 1; op < nbase - zeros; ++op)
                        idx[n++] = LLVMGetOperand(ptr, op);
                    LLVMPositionBuilderBefore(gen->builder, inst);
                    LLVMSetOperand(inst, ptrop, LLVMBuildInBoundsGEP2(gen->builder,
                        LLVMGetGEPSourceElementType(ptr), LLVMGetOperand(ptr, 0), idx, n, ""));
                }
            }
            if (depth <= 0) {
                inst = next;
                continue;
            }
            LLVMValueRef idx[64];
            if (depth > 60) {
                inst = next;
                continue;
            }
            LLVMPositionBuilderBefore(gen->builder, inst);
            if (LLVMIsAGetElementPtrInst(inst)) {
                // An address computation starting at its pointer, not from
                // it, is rebuilt from the type its pointer is known to point to
                int nops = LLVMGetNumOperands(inst);
                LLVMValueRef first = LLVMGetOperand(inst, 1);
                if (!LLVMIsAConstantInt(first) || LLVMConstIntGetZExtValue(first) != 0 || depth + nops > 60) {
                    inst = next;
                    continue;
                }
                int n = 0;
                idx[n++] = first;
                for (int d = 0; d < depth; ++d)
                    idx[n++] = zero;
                for (int op = 2; op < nops; ++op)
                    idx[n++] = LLVMGetOperand(inst, op);
                LLVMValueRef gep = LLVMIsInBounds(inst)
                    ? LLVMBuildInBoundsGEP2(gen->builder, pointee, ptr, idx, n, "")
                    : LLVMBuildGEP2(gen->builder, pointee, ptr, idx, n, "");
                LLVMReplaceAllUsesWith(inst, gep);
                LLVMInstructionEraseFromParent(inst);
            }
            else {
                idx[0] = zero;
                for (int d = 0; d < depth; ++d)
                    idx[d + 1] = zero;
                LLVMSetOperand(inst, ptrop, LLVMBuildInBoundsGEP2(gen->builder, pointee, ptr, idx, depth + 1, ""));
            }
            inst = next;
        }
    }
}

// LLVM 23's SPIR-V backend crashes in its pointer-cast legalisation on an
// address computed from one made in another block, from a parameter, once
// the function has returned early twice. So on a GPU target an address
// computed from another starting at it (its first index zero) is computed
// in one step from where that one starts, as instcombine would have it; and
// one computed by stepping from another made in another block computes that
// other again just before it.
static void genlGpuGepChainsFn(GenState *gen, LLVMValueRef fn) {
    for (LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(fn); blk; blk = LLVMGetNextBasicBlock(blk)) {
        LLVMValueRef inst = LLVMGetFirstInstruction(blk);
        while (inst) {
            LLVMValueRef next = LLVMGetNextInstruction(inst);
            LLVMValueRef base = LLVMIsAGetElementPtrInst(inst) ? LLVMGetOperand(inst, 0) : NULL;
            if (!base || !LLVMIsAGetElementPtrInst(base)) {
                inst = next;
                continue;
            }
            LLVMValueRef first = LLVMGetOperand(inst, 1);
            int nbase = LLVMGetNumOperands(base);
            int nops = LLVMGetNumOperands(inst);
            if (LLVMIsAConstantInt(first) && LLVMConstIntGetZExtValue(first) == 0
                && LLVMGetGEPSourceElementType(inst) == genlGepResultType(base) && nbase + nops <= 64) {
                LLVMValueRef idx[64];
                int n = 0;
                for (int op = 1; op < nbase; ++op)
                    idx[n++] = LLVMGetOperand(base, op);
                for (int op = 2; op < nops; ++op)
                    idx[n++] = LLVMGetOperand(inst, op);
                LLVMPositionBuilderBefore(gen->builder, inst);
                LLVMTypeRef type = LLVMGetGEPSourceElementType(base);
                LLVMValueRef ptr = LLVMGetOperand(base, 0);
                LLVMValueRef gep = LLVMIsInBounds(inst) && LLVMIsInBounds(base)
                    ? LLVMBuildInBoundsGEP2(gen->builder, type, ptr, idx, n, "")
                    : LLVMBuildGEP2(gen->builder, type, ptr, idx, n, "");
                LLVMReplaceAllUsesWith(inst, gep);
                LLVMInstructionEraseFromParent(inst);
                if (!LLVMGetFirstUse(base))
                    LLVMInstructionEraseFromParent(base);
                // The merged address may itself start from another
                inst = gep;
                continue;
            }
            // Stepping from an address made elsewhere: make it again here
            LLVMValueRef user = inst;
            while (LLVMIsAGetElementPtrInst(base) && LLVMGetInstructionParent(base) != blk) {
                LLVMValueRef copy = LLVMInstructionClone(base);
                LLVMPositionBuilderBefore(gen->builder, user);
                LLVMInsertIntoBuilder(gen->builder, copy);
                LLVMSetOperand(user, 0, copy);
                if (!LLVMGetFirstUse(base))
                    LLVMInstructionEraseFromParent(base);
                user = copy;
                base = LLVMGetOperand(copy, 0);
            }
            inst = next;
        }
    }
}

static void genlGpuAggregates(GenState *gen) {
    GenlAggMap map;
    map.mask = 255;
    map.used = 0;
    map.slots = (GenlAggSlot *)calloc(map.mask + 1, sizeof(GenlAggSlot));
    LLVMSetCurrentDebugLocation2(gen->builder, NULL);
    for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
        if (LLVMIsDeclaration(fn))
            continue;
        genlGpuBufferAccess(gen, fn);
        genlGpuAggregatesFn(gen, &map, fn);
        genlGpuRetypeFn(gen, fn);
        genlGpuGepChainsFn(gen, fn);
        for (uint32_t i = 0; i <= map.mask; ++i)
            free(map.slots[i].leaves);
        memset(map.slots, 0, (map.mask + 1) * sizeof(GenlAggSlot));
        map.used = 0;
    }
    free(map.slots);
}

// Generate IR nodes into LLVM IR using LLVM
void genpgm(GenState *gen, ProgramNode *pgm) {
    char *err;

    // Generate IR to LLVM IR
    genlProgram(gen, pgm);
    if (gen->opt->gpu && !errors)
        genlGpuCalls(gen);

    // Generation reports only what makes an object wrong -- a C name declared
    // two ways (genlClaimSymbol); on a GPU target, a global holding a borrow
    // (genlGloVar) or a function calling itself (genlGpuCalls) -- so nothing
    // is emitted after one
    if (errors) {
        LLVMDisposeModule(gen->module);
        return;
    }

    // Every aggregate value too large to carry whole is moved into memory and
    // copied there (genlaggcopy.c). Not on a GPU target, where 'memcpy' is
    // not legal and its own pipeline breaks every aggregate into scalars
    if (!gen->opt->gpu)
        genlAggCopies(gen);

    // Verify generated IR
    if (gen->opt->verify) {
        timerBegin(VerifyTimer);
        char *error = NULL;
        LLVMVerifyModule(gen->module, LLVMReturnStatusAction, &error);
        if (error) {
            if (*error)
                errorMsg(ErrorGenErr, "Module verification failed:\n%s", error);
            LLVMDisposeMessage(error);
        }
    }

    // Serialize the LLVM IR, if requested
    if (gen->opt->print_llvmir && LLVMPrintModuleToFile(gen->module, fileMakePath(gen->opt->output, gen->opt->srcname, "preir"), &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit pre-ir file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Optimize the generated LLVM IR, through LLVM's new pass manager. A
    // release build runs LLVM's standard O2 pipeline (what clang's -O2 runs:
    // inlining, SROA, instcombine, LICM, unrolling, the loop and SLP
    // vectorizers), given the target machine, so that every cost model is the
    // CPU's that --cpu and --features name. O3 was measured against it and
    // compiled slower for no run time worth having. Generation marks no float
    // operation fast or contractable, so nothing reorders a float sum. A debug
    // build only promotes allocas to registers, reassociates, eliminates
    // common subexpressions and simplifies the control flow graph, each
    // function in turn, with no target machine.
    //
    // A GPU target's pipeline is its own at every optimization level, debug
    // too, since without it the module is not valid SPIR-V: inline every
    // call (genlGpuCalls), break each struct and array into separate values
    // (so a struct holding a reference dissolves into locals), and infer each
    // pointer's address space from its origin, given the target machine
    // (genlLLVMOptions names the flat space); then fold what that leaves,
    // with instsimplify and not instcombine, which rewrites a field's address
    // as a byte offset from its struct's, an address LLVM 23's SPIR-V backend
    // crashes on; and last, make the control flow structured, each branch
    // merging before the next is taken (structurizecfg), since that backend's
    // own structurizer leaves a chain of early returns, or a loop's body
    // returning early, as a module the validator refuses. structurizecfg
    // takes no switch, and makes each of a switch's branches 'br i1 undef',
    // so a switch is first made a tree of branches (lower-switch): simplifycfg
    // makes one of an if-elif chain on one integer. And every loop is first
    // given a preheader (loop-simplify), as LLVM's own GPU pipelines give
    // structurizecfg every loop: simplifycfg folds away the empty block
    // before a loop, and a loop entered straight from a conditional branch
    // (a loop in an 'if', a failed check leaving it by another way) is
    // structured with a back edge that always exits, so the loop runs once
    // and what follows it is skipped. A release build adds
    // the usual optimizations. Then what is left of a struct or array value
    // is carried as its scalar leaves, and every field's address is computed
    // from its struct's type (genlGpuAggregates).
    //
    // A kernel is settled between the two halves of the GPU pipeline
    // (genlGpuEntries): once everything is inlined into it, and before its
    // control flow is structured, since a failed check's record ends in a
    // return of its own. What that leaves unused goes with it.
    timerBegin(OptTimer);
    const char *pipeline = gen->opt->gpu
        ? (gen->opt->release
            ? "always-inline,function(sroa,infer-address-spaces,instsimplify,reassociate,gvn,simplifycfg)"
            : "always-inline,function(sroa,infer-address-spaces,instsimplify,simplifycfg)")
        : gen->opt->release
        ? "default<O2>"
        : "function(mem2reg,reassociate,gvn,simplifycfg)";
    LLVMPassBuilderOptionsRef passopts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef passerr = LLVMRunPasses(gen->module, pipeline,
        gen->opt->gpu || gen->opt->release ? gen->machine : NULL, passopts);
    int refused = 0;
    if (gen->opt->gpu && !passerr) {
        int before = errors;
        if (gen->entrycnt > 0)
            genlGpuEntries(gen);
        refused = errors != before;
        if (!refused)
            passerr = LLVMRunPasses(gen->module,
                gen->entrycnt > 0 ? "globaldce,function(infer-address-spaces,instsimplify,adce,lower-switch,loop-simplify,structurizecfg)"
                    : "function(lower-switch,loop-simplify,structurizecfg)",
                gen->machine, passopts);
    }
    LLVMDisposePassBuilderOptions(passopts);
    if (passerr) {
        char *msg = LLVMGetErrorMessage(passerr);
        errorMsg(ErrorGenErr, "Could not optimize: %s", msg);
        LLVMDisposeErrorMessage(msg);
    }
    // A kernel's slice that came from nowhere it can be indexed was refused
    // (genlGpuEntries), and nothing is emitted
    if (refused) {
        LLVMDisposeModule(gen->module);
        return;
    }
    if (gen->opt->gpu && !passerr) {
        genlGpuAggregates(gen);
        // Each atomic's scope and ordering, from the memory it acts on; one in
        // a kernel on memory no other invocation reaches is refused, and
        // nothing is emitted
        int atomicsok = 1;
        for (LLVMValueRef fn = LLVMGetFirstFunction(gen->module); fn; fn = LLVMGetNextFunction(fn)) {
            if (LLVMIsDeclaration(fn))
                continue;
            FnDclNode *kernel = NULL;
            for (uint32_t e = 0; e < gen->entrycnt; ++e)
                if (gen->entries[e].kernel == fn)
                    kernel = gen->entries[e].fndcl;
            atomicsok = genlGpuAtomics(gen, fn, kernel) && atomicsok;
        }
        if (!atomicsok) {
            LLVMDisposeModule(gen->module);
            return;
        }
    }

    // Serialize the LLVM IR, if requested
    if (gen->opt->print_llvmir && LLVMPrintModuleToFile(gen->module, fileMakePath(gen->opt->output, gen->opt->srcname, "ir"), &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Transform IR to target's ASM and OBJ. A SPIR-V target's object is a
    // SPIR-V module, named as shader tools name one
    timerBegin(CodeGenTimer);
    if (gen->machine) {
        char *objfile = gen->opt->wasm? "wasm" : gen->opt->gpu? "spv" : objext;
        char *asmfile = gen->opt->wasm? "wat" : gen->opt->gpu? "spvasm" : asmext;
        char *objpath = fileMakePath(gen->opt->output, gen->opt->srcname, objfile);
        char *asmpath = gen->opt->print_asm? fileMakePath(gen->opt->output, gen->opt->srcname, asmfile) : NULL;
        if (gen->opt->vulkan)
            genlGpuOut(gen, objpath, asmpath);
        else
            genlOut(objpath, asmpath, gen->module, gen->machine);
    }

    LLVMDisposeModule(gen->module);
    // LLVMContextDispose(gen.context);  // Only need if we created a new context
}

// Setup LLVM generation, ensuring we know intended target
// Which COMDAT selection kinds this target's object format will lower. Both
// restrictions are hard errors inside LLVM's backend rather than something it
// works around, and the C API exposes no object-format query, so ask the triple.
static int genlComdatSupport(char *triple) {
    if (strstr(triple, "wasm") || strstr(triple, "emscripten"))
        return ComdatMergeOnly;
    // Mach-O needs none: its assembler emits .subsections_via_symbols, which
    // already lets the linker strip a symbol at a time
    if (strstr(triple, "darwin") || strstr(triple, "apple")
        || strstr(triple, "macos") || strstr(triple, "ios"))
        return ComdatNone;
    return ComdatFull;
}

// Hand LLVM the command-line options named by the environment variable
// CONE_LLVM_OPTIONS, separated by spaces, as a testing aid: for instance
// '-force-opaque-pointers', which LLVM 13 reads when it creates its context.
// So this runs before anything creates one.
//
// A GPU target adds one of its own. Every Cone reference is in address space
// 0, and a global's address is cast there where it is taken (genlVarSym), for
// LLVM's address-space inference to undo once every call is inlined. That
// inference rewrites only casts into the target's flat address space, which
// for SPIR-V's Vulkan form is 0 but for its OpenCL form is 4 (Generic), so 0
// is named the flat space for both. And machine CSE is off: LLVM 23's SPIR-V
// backend lets it hoist a computation both successors of a loop's header make
// into the header, after the OpLoopMerge it has already placed there, which
// must come just before the branch (a loop whose body and exit both scale a
// struct's field, once the struct is broken into values, is refused by the
// validator in a release build). Two of LLVM's code generation passes for a
// CPU are off too, since each rewrites addresses into what that backend
// crashes on under SPIR-V's logical addressing: loop strength reduction
// walks an array in a loop by a pointer stepped a byte count at a time; and
// after CodeGenPrepare, a field's address computed from a parameter in one
// block and used in another crashes its pointer-cast legalisation, and a loop
// whose body exits early from within a branch crashes its region splitting.
static void genlLLVMOptions(int gpu) {
    char *env = getenv("CONE_LLVM_OPTIONS");
    const char *argv[64];
    int argc = 0;
    argv[argc++] = "conec";
    if (gpu) {
        argv[argc++] = "-assume-default-is-flat-addrspace";
        argv[argc++] = "-disable-machine-cse";
        argv[argc++] = "-disable-lsr";
        argv[argc++] = "-disable-cgp";
    }
    if (env != NULL && *env != '\0') {
        char *opts = memAllocStr(env, strlen(env));
        char *next = strtok(opts, " ");
        while (next && argc < 64) {
            argv[argc++] = next;
            next = strtok(NULL, " ");
        }
    }
    if (argc > 1)
        LLVMParseCommandLineOptions(argc, argv, "");
}

void genSetup(GenState *gen, ConeOptions *opt) {
    gen->opt = opt;
    gen->libroot = NULL;
    // A SPIR-V triple is a GPU target (the default triple is the host's, never
    // one). Type check's flow reads this before anything is generated.
    opt->gpu = opt->triple != NULL && strncmp(opt->triple, "spirv", 5) == 0;
    opt->vulkan = opt->gpu && strstr(opt->triple, "vulkan") != NULL;
    genlLLVMOptions(opt->gpu);
    // A crash inside LLVM then names the pass and the function it was in, as
    // llc's does, rather than ending conec without a word
    LLVMEnablePrettyStackTrace();

    LLVMTargetMachineRef machine = genlCreateMachine(opt);
    if (!machine)
        exit(ExitOpts);

    // Obtain data layout info, particularly pointer sizes
    gen->machine = machine;
    gen->datalayout = LLVMCreateTargetDataLayout(machine);
    // SPIR-V's Vulkan form addresses logically, with no pointer to measure,
    // and WebGPU has no 64-bit integer: a GPU's sizes and indices are 32 bits
    // there (WGSL's 'arrayLength' is a u32), so usize is
    opt->ptrsize = opt->vulkan ? 32 : LLVMPointerSize(gen->datalayout) << 3;

    gen->context = LLVMContextCreate();
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    gen->fn = NULL;
    gen->fnblock = NULL;
    gen->fndcl = NULL;
    gen->exitzero = 0;
    gen->allocaPoint = NULL;
    gen->blockstack = memAllocBlk(sizeof(GenBlockState)*GenBlockStackMax);
    gen->blockstackcnt = 0;
    gen->temps = NULL;
    gen->tempcnt = 0;
    gen->tempmax = 0;
    gen->tempbase = 0;
    gen->seams = NULL;
    gen->resumeat = NULL;
    gen->resumeblk = NULL;
    gen->resumedest = NULL;
    gen->resumeheld = NULL;
    gen->resumeheldcnt = 0;
    gen->flights = NULL;
    gen->flightcnt = 0;
    gen->flightmax = 0;
    gen->flightbase = 0;

    gen->comdats = genlComdatSupport(opt->triple);   // genlCreateMachine filled in the default
    gen->cabi = genlCAbiTarget(opt->triple);
    gen->emptyStructType = genlEmptyStruct(gen);
}

void genClose(GenState *gen) {
    LLVMDisposeTargetMachine(gen->machine);
}
