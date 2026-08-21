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

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/Scalar.h>
#include <llvm-c/Transforms/IPO.h>
#if LLVM_VERSION_MAJOR >= 7
#include "llvm-c/Transforms/Utils.h"
#endif

#include <stdio.h>
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

// Generate a function
void genlFn(GenState *gen, FnDclNode *fnnode) {
    // Only a concrete declaration has an implementation. An overload name is a
    // namespace binding that selection replaces long before generation.
    assert(fnnode->tag == FnDclTag && "Only a concrete function/method is generated");
    if ((fnnode->flags & FlagInline) || fnnode->value->tag == IntrinsicTag)
        return;

    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    INode *svfnblock = gen->fnblock;

    FnSigNode *fnsig = (FnSigNode*)fnnode->vtype;
    assert(fnnode->value->tag == BlockTag);
    gen->fn = fnnode->llvmvar;
    gen->fnblock = fnnode->value;

    // Attach block and builder to function
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
    gen->builder = LLVMCreateBuilder();
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

    // Generate the function's code (always a block)
    genlBlock(gen, (BlockNode *)fnnode->value);

	// erase temporary dummy alloca inserted earlier
    if (LLVMGetInstructionParent(allocaPoint))
        LLVMInstructionEraseFromParent(allocaPoint);

    LLVMDisposeBuilder(gen->builder);

    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fnblock = svfnblock;
}

// Insert every alloca before the allocaPoint in the function's entry block.
// Why? To improve LLVM optimization of SRoA and mem2reg, all allocas
// should be located in the function's entry block before the first call.
LLVMValueRef genlAlloca(GenState *gen, LLVMTypeRef type, const char *name) {
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gen->builder);
    LLVMPositionBuilderBefore(gen->builder, gen->allocaPoint);
    LLVMValueRef alloca = LLVMBuildAlloca(gen->builder, type, name);
    LLVMPositionBuilderAtEnd(gen->builder, current_block);
    return alloca;
}

// Generate global variable
void genlGloVar(GenState *gen, VarDclNode *varnode) {
    if (!varnode->value) {
        // If no value on non-extern, initialize with the zero initializer
        if (!(varnode->flags & FlagExtern))
            LLVMSetInitializer(varnode->llvmvar, LLVMConstNull(genlType(gen,varnode->vtype)));
        return;
    }

    else if (varnode->value->tag == StringLitTag) {
        SLitNode *strnode = (SLitNode*)varnode->value;
        LLVMSetInitializer(varnode->llvmvar, LLVMConstStringInContext(gen->context, strnode->strlit, strnode->strlen, 1));
    }
    else
        LLVMSetInitializer(varnode->llvmvar, genlExpr(gen, varnode->value));

    // Mark initialized, immutable global variable as constant,
    // so it goes into a faster memory page that we know we will never be mutated
    if (itypeGetTypeDcl(varnode->perm) == (INode*)immPerm)
        LLVMSetGlobalConstant(varnode->llvmvar, 1);
}

// Generate LLVMValueRef for a global variable
// It sets appropriate visibility, linkage and constant flags for the linker
void genlGloVarName(GenState *gen, VarDclNode *glovar) {
    glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), glovar->genname);

    // Mark immutable global variables as 'constant', so they can appear in immutable blocks
    // This improves performance
    if (permIsSame(glovar->perm, (INode*) immPerm))
        LLVMSetGlobalConstant(glovar->llvmvar, 1);

    // Private global variables are not visible to other modules
    if (glovar->namesym && glovar->namesym->namestr == '_')
        LLVMSetVisibility(glovar->llvmvar, LLVMHiddenVisibility);
}

// Create mangled function name for a generic instantiation.
// A concrete function/method needs no signature mangling: its source name is
// unique in its namespace, and its generated name already carries that namespace.
char *genlMangleMethName(char *workbuf, FnDclNode *node) {
    if (node->instnode == NULL)
        return node->genname;

    strcat(workbuf, node->genname);
    char *bufp = workbuf + strlen(workbuf);

    FnSigNode *fnsig = (FnSigNode *)node->vtype;
    uint32_t cnt;
    INode **nodesp;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        *bufp++ = ':';
        bufp = itypeMangle(bufp, ((IExpNode *)*nodesp)->vtype);
    }
    *bufp = '\0';

    return workbuf;
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
        char workbuf[2048] = { '\0' };
        char *manglednm = genlMangleMethName(workbuf, glofn);
        char *fnname = glofn->namesym? &glofn->namesym->namestr : "";
        glofn->llvmvar = LLVMAddFunction(gen->module, manglednm, genlType(gen, glofn->vtype));

        // Specify appropriate storage class, visibility and call convention
        // extern functions (linkedited in separately):
        if (glofn->flags & FlagSystem) {
            LLVMSetFunctionCallConv(glofn->llvmvar, LLVMX86StdcallCallConv);
            LLVMSetDLLStorageClass(glofn->llvmvar, LLVMDLLImportStorageClass);
        }
        // else if glofn-flags involve dynamic library export:
        //    LLVMSetDLLStorageClass(glofn->llvmvar, LLVMDLLExportStorageClass); 
        else if (fnname[0] == '_') {
            // Private globals should be hidden. (public globals have DefaultVisibility)            
            LLVMSetVisibility(glofn->llvmvar, LLVMHiddenVisibility);
        }

        // Add metadata on implemented functions (debug mode only)
        if (!gen->opt->release && glofn->value) {
            LLVMMetadataRef fntype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder,
                gen->difile, NULL, 0, 0);
            LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
                fnname, strlen(fnname), manglednm, strlen(manglednm),
                gen->difile, glofn->linenbr, fntype, 0, 1, glofn->linenbr, LLVMDIFlagPublic, 0);
            LLVMSetSubprogram(glofn->llvmvar, sp);
        }
    }
}

void genlGlobalSyms(GenState *gen, INode *node);

// Generate the global symbols for one instance of a generic type, giving each
// method the linkage that lets the linker keep one copy across object files --
// the same treatment a generic function's instances get below.
static void genlGenericInstanceSyms(GenState *gen, INode *instance) {
    if (instance->tag != StructTag || (instance->flags & TraitType))
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((INsTypeNode*)instance)->nodelist, cnt, nodesp)) {
        genlGlobalSyms(gen, *nodesp);
        if ((*nodesp)->tag == FnDclTag && ((FnDclNode*)*nodesp)->llvmvar)
            LLVMSetLinkage(((FnDclNode*)*nodesp)->llvmvar, LLVMLinkOnceAnyLinkage);
    }
}

// Generate module or type global symbols
void genlGlobalSyms(GenState *gen, INode *node) {
    // Handle type nodes
    if (isTypeNode(node)) {
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
        if (isMethodType(node) && !(node->tag == StructTag && (node->flags & TraitType))) {
            INsTypeNode *tnode = (INsTypeNode*)node;
            INode **nodesp;
            uint32_t cnt;
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
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
                FnDclNode *fnnode = (FnDclNode *)*nodesp;
                genlGloFnName(gen, fnnode);
                // Ensure that linker only picks one generic instantiation across multiple object files
                LLVMSetLinkage(fnnode->llvmvar, LLVMLinkOnceAnyLinkage);
            }
        }
        else
            genlGloFnName(gen, (FnDclNode *)node);
        break;
    // An overload name has no symbol of its own, but it does make its candidates
    // reachable. A public overload name in an imported module may select a
    // private candidate, whose own namespace entry the program's privacy filter
    // skips, so generate every candidate's name here too.
    case FnOverloadDclTag: {
        uint32_t ovlcnt;
        INode **ovlnodesp;
        for (nodesFor(((FnOverloadDclNode*)node)->overloads, ovlcnt, ovlnodesp))
            genlGlobalSyms(gen, *ovlnodesp);
        break;
    }
    }
}

// Generate module or type implementation (e.g., function blocks)
void genlGlobalImpl(GenState *gen, INode *node) {
    // Handle type nodes
    if (isTypeNode(node)) {
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
        // For types with a namespace, let's do its nodes too
        if (isMethodType(node) && !(node->tag == StructTag && (node->flags & TraitType))) {
            INsTypeNode *tnode = (INsTypeNode*)node;
            INode **nodesp;
            uint32_t cnt;
            for (nodelistFor(&tnode->nodelist, cnt, nodesp)) {
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
        break;

    default:
        assert(0 && "Invalid global area node");
        break;
    }
}

// Generate the program
void genlProgram(GenState *gen, ProgramNode *pgm) {

    assert(pgm->tag == ProgramTag);
    gen->module = LLVMModuleCreateWithNameInContext(gen->opt->srcname, gen->context);
    if (!gen->opt->release) {
        gen->dibuilder = LLVMCreateDIBuilder(gen->module);
        gen->difile = LLVMDIBuilderCreateFile(gen->dibuilder, gen->opt->srcpath, strlen(gen->opt->srcpath), ".", 1);
        // The compile unit is attached to the module; nothing reads it back
        LLVMDIBuilderCreateCompileUnit(gen->dibuilder, LLVMDWARFSourceLanguageC,
            gen->difile, "Cone compiler", 13, 0, "", 0, 0, "", 0, LLVMDWARFEmissionFull, 0, 0, 0, "", 0, "", 0);
    }

    // First, generate global symbols for all modules, so that forward references succeed
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        ModuleNode *mod = (ModuleNode*)*nodesp;
        int16_t generating = mod->flags & FlagGenMod;

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

        // Generate implementation only for module(s) flagged for generation
        if (mod->flags & FlagGenMod) {
            uint32_t icnt;
            INode **inodesp;
            for (nodesFor(mod->nodes, icnt, inodesp)) {
                genlGlobalImpl(gen, *inodesp);
            }
        }
    }

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
void genlOut(char *objpath, char *asmpath, LLVMModuleRef mod, char *triple, LLVMTargetMachineRef machine) {
    char *err;
    LLVMTargetDataRef dataref;
    char *layout;

    LLVMSetTarget(mod, triple);
    dataref = LLVMCreateTargetDataLayout(machine);
    layout = LLVMCopyStringRepOfTargetData(dataref);
    LLVMSetDataLayout(mod, layout);
    LLVMDisposeMessage(layout);

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

    // Optimize the generated LLVM IR
    timerBegin(OptTimer);
    LLVMPassManagerRef passmgr = LLVMCreatePassManager();
    LLVMAddPromoteMemoryToRegisterPass(passmgr);     // Demote allocas to registers.
    //LLVMAddInstructionCombiningPass(passmgr);        // Do simple "peephole" and bit-twiddling optimizations
    LLVMAddReassociatePass(passmgr);                 // Reassociate expressions.
    LLVMAddGVNPass(passmgr);                         // Eliminate common subexpressions.
    LLVMAddCFGSimplificationPass(passmgr);           // Simplify the control flow graph
    if (gen->opt->release)
        LLVMAddFunctionInliningPass(passmgr);        // Function inlining
    LLVMRunPassManager(passmgr, gen->module);
    LLVMDisposePassManager(passmgr);

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
            gen->module, gen->opt->triple, gen->machine);

    LLVMDisposeModule(gen->module);
    // LLVMContextDispose(gen.context);  // Only need if we created a new context
}

// Setup LLVM generation, ensuring we know intended target
void genSetup(GenState *gen, ConeOptions *opt) {
    gen->opt = opt;

    LLVMTargetMachineRef machine = genlCreateMachine(opt);
    if (!machine)
        exit(ExitOpts);

    // Obtain data layout info, particularly pointer sizes
    gen->machine = machine;
    gen->datalayout = LLVMCreateTargetDataLayout(machine);
    opt->ptrsize = LLVMPointerSize(gen->datalayout) << 3;

    gen->context = LLVMGetGlobalContext(); // LLVM inlining bugs prevent use of LLVMContextCreate();
    gen->builder = LLVMCreateBuilder();
    gen->fn = NULL;
    gen->fnblock = NULL;
    gen->allocaPoint = NULL;
    gen->blockstack = memAllocBlk(sizeof(GenBlockState)*GenBlockStackMax);
    gen->blockstackcnt = 0;

    gen->emptyStructType = genlEmptyStruct(gen);
}

void genClose(GenState *gen) {
    LLVMDisposeTargetMachine(gen->machine);
}
