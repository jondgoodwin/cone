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

// Generate parameter variable
void genlParmVar(GenState *gen, VarDclNode *var) {
    assert(var->tag == VarDclTag);
    // We always alloca in case variable is mutable or we want to take address of its value
    var->llvmvar = genlAlloca(gen, genlType(gen, var->vtype), &var->namesym->namestr);
    LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, var->index), var->llvmvar);
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
    int svexitzero = gen->exitzero;

    FnSigNode *fnsig = (FnSigNode*)fnnode->vtype;
    assert(fnnode->value->tag == BlockTag);
    gen->fn = fnnode->llvmvar;
    gen->fnblock = fnnode->value;
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
        genlParmVar(gen, (VarDclNode*)*nodesp);

    // Generate the function's code (always a block). A drop the compiler gave
    // a type is built here, from the type's layout: an enum's block is empty,
    // and a struct's holds only its 'final' calls.
    if (structIsGeneratedDropFn((INode*)fnnode))
        genlTypeDrop(gen, fnnode);
    else
        genlBlock(gen, (BlockNode *)fnnode->value);

	// erase temporary dummy alloca inserted earlier
    if (LLVMGetInstructionParent(allocaPoint))
        LLVMInstructionEraseFromParent(allocaPoint);

    LLVMDisposeBuilder(gen->builder);

    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fnblock = svfnblock;
    gen->exitzero = svexitzero;
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
// it to that 'final' as '&uni', which may write it.
static int genlGloVarIsConstant(VarDclNode *glovar) {
    return permIsSame(glovar->perm, (INode*)immPerm) && glovar->value != NULL
        && !(glovar->dclinfo.facts & DclExternal)
        && itypeGetDropFnDcl(glovar->vtype) == NULL;
}

// Generate global variable
void genlGloVar(GenState *gen, VarDclNode *varnode) {
    LLVMValueRef global = genlGloVarGlobal(varnode);

    // An extern global is defined in some other object file; this one only
    // names it, and a declaration may not lead a COMDAT
    if (!(varnode->dclinfo.facts & DclExternal))
        genlComdat(gen, global);

    if (!varnode->value) {
        // If no value on non-extern, initialize with the zero initializer
        if (!(varnode->dclinfo.facts & DclExternal))
            LLVMSetInitializer(global, LLVMConstNull(genlType(gen,varnode->vtype)));
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
// convention, or two globals of one LLVM value type and one permission
static int genlSymAgree(GenState *gen, INode *a, INode *b) {
    if (a->tag != b->tag)
        return 0;
    if (a->tag == FnDclTag)
        return genlType(gen, ((FnDclNode*)a)->vtype) == genlType(gen, ((FnDclNode*)b)->vtype)
            && (inodeGetDclInfo(a)->facts & DclSystemCC) == (inodeGetDclInfo(b)->facts & DclSystemCC);
    VarDclNode *va = (VarDclNode*)a;
    VarDclNode *vb = (VarDclNode*)b;
    return genlType(gen, va->vtype) == genlType(gen, vb->vtype) && permIsSame(va->perm, vb->perm);
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
    // node: an LLVM intrinsic it calls by name ('llvm.trap', genlPanic). A
    // declaration meeting one shares it where it can
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
            "The C name %s is declared differently at %s:%u. Every declaration of one C name must agree: a function in its signature, a global in its type and permission.",
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
        glovar->llvmvar = LLVMConstBitCast(global, LLVMPointerType(vartype, 0));
    }
    else
        global = glovar->llvmvar = LLVMAddGlobal(gen->module, vartype, nameSymbol(symbol, (INode*)glovar));

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
        LLVMTypeRef fntype = genlType(gen, glofn->vtype);
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
        GenlDefinition defined = genlDefinition(gen, (INode*)glofn);
        genlLinkage(glofn->llvmvar, (INode*)glofn, defined);
        genlClaimSymbol(gen, (INode*)glofn, glofn->llvmvar, defined, symbol);

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
    if (!opt->cpu)
        opt->cpu = "generic";
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

    // Generate assembly file if requested
    if (asmpath && LLVMTargetMachineEmitToFile(machine, mod, asmpath, LLVMAssemblyFile, &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit asm file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Generate .o or .obj file
    if (LLVMTargetMachineEmitToFile(machine, mod, objpath, LLVMObjectFile, &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
        LLVMDisposeMessage(err);
    }
}

// Generate IR nodes into LLVM IR using LLVM
void genpgm(GenState *gen, ProgramNode *pgm) {
    char *err;

    // Generate IR to LLVM IR
    genlProgram(gen, pgm);

    // Generation reports only what makes an object wrong -- a C name declared
    // two ways (genlClaimSymbol) -- so nothing is emitted after one
    if (errors) {
        LLVMDisposeModule(gen->module);
        return;
    }

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

    // Optimize the generated LLVM IR, through LLVM's new pass manager. Each
    // function in turn: promote allocas to registers, reassociate expressions,
    // eliminate common subexpressions, and simplify the control flow graph.
    // Then, in a release build only, inline. No target machine is given, so the
    // inliner's costs are the target-independent ones.
    timerBegin(OptTimer);
    const char *pipeline = gen->opt->release
        ? "function(mem2reg,reassociate,gvn,simplifycfg),cgscc(inline)"
        : "function(mem2reg,reassociate,gvn,simplifycfg)";
    LLVMPassBuilderOptionsRef passopts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef passerr = LLVMRunPasses(gen->module, pipeline, NULL, passopts);
    LLVMDisposePassBuilderOptions(passopts);
    if (passerr) {
        char *msg = LLVMGetErrorMessage(passerr);
        errorMsg(ErrorGenErr, "Could not optimize: %s", msg);
        LLVMDisposeErrorMessage(msg);
    }

    // Serialize the LLVM IR, if requested
    if (gen->opt->print_llvmir && LLVMPrintModuleToFile(gen->module, fileMakePath(gen->opt->output, gen->opt->srcname, "ir"), &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Transform IR to target's ASM and OBJ
    timerBegin(CodeGenTimer);
    if (gen->machine)
        genlOut(fileMakePath(gen->opt->output, gen->opt->srcname, gen->opt->wasm? "wasm" : objext),
            gen->opt->print_asm? fileMakePath(gen->opt->output, gen->opt->srcname, gen->opt->wasm? "wat" : asmext) : NULL,
            gen->module, gen->machine);

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
static void genlLLVMOptions() {
    char *env = getenv("CONE_LLVM_OPTIONS");
    if (env == NULL || *env == '\0')
        return;
    char *opts = memAllocStr(env, strlen(env));
    const char *argv[64];
    int argc = 0;
    argv[argc++] = "conec";
    char *next = strtok(opts, " ");
    while (next && argc < 64) {
        argv[argc++] = next;
        next = strtok(NULL, " ");
    }
    LLVMParseCommandLineOptions(argc, argv, "");
}

void genSetup(GenState *gen, ConeOptions *opt) {
    gen->opt = opt;
    gen->libroot = NULL;
    genlLLVMOptions();

    LLVMTargetMachineRef machine = genlCreateMachine(opt);
    if (!machine)
        exit(ExitOpts);

    // Obtain data layout info, particularly pointer sizes
    gen->machine = machine;
    gen->datalayout = LLVMCreateTargetDataLayout(machine);
    opt->ptrsize = LLVMPointerSize(gen->datalayout) << 3;

    gen->context = LLVMContextCreate();
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    gen->fn = NULL;
    gen->fnblock = NULL;
    gen->exitzero = 0;
    gen->allocaPoint = NULL;
    gen->blockstack = memAllocBlk(sizeof(GenBlockState)*GenBlockStackMax);
    gen->blockstackcnt = 0;

    gen->comdats = genlComdatSupport(opt->triple);   // genlCreateMachine filled in the default
    gen->emptyStructType = genlEmptyStruct(gen);
}

void genClose(GenState *gen) {
    LLVMDisposeTargetMachine(gen->machine);
}
