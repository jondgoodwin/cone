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
    var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, var->vtype), &var->namesym->namestr);
    LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, var->index), var->llvmvar);
}

// Generate a function
void genlFn(GenState *gen, FnDclNode *fnnode) {
    if (fnnode->value->tag == IntrinsicTag)
        return;

    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    FnSigNode *fnsig = (FnSigNode*)fnnode->vtype;

    assert(fnnode->value->tag == BlockTag);
    gen->fn = fnnode->llvmvar;

    // Attach block and builder to function
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
    gen->builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(gen->builder, entry);

    // Generate LLVMValueRef's for all parameters, so we can use them as local vars in code
    uint32_t cnt;
    INode **nodesp;
    for (nodesFor(fnsig->parms, cnt, nodesp))
        genlParmVar(gen, (VarDclNode*)*nodesp);

    // Generate the function's code (always a block)
    genlBlock(gen, (BlockNode *)fnnode->value);

    LLVMDisposeBuilder(gen->builder);

    gen->builder = svbuilder;
    gen->fn = svfn;
}

// Generate global variable
void genlGloVar(GenState *gen, VarDclNode *varnode) {
    if (varnode->value->tag == StrLitTag) {
        SLitNode *strnode = (SLitNode*)varnode->value;
        ArrayNode *anode = (ArrayNode*)strnode->vtype;
        LLVMSetInitializer(varnode->llvmvar, LLVMConstStringInContext(gen->context, strnode->strlit, anode->size, 1));
    }
    else
        LLVMSetInitializer(varnode->llvmvar, genlExpr(gen, varnode->value));
}

// Generate LLVMValueRef for a global variable
void genlGloVarName(GenState *gen, VarDclNode *glovar) {
    glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), glovar->genname);
    if (permIsSame(glovar->perm, (INode*) immPerm))
        LLVMSetGlobalConstant(glovar->llvmvar, 1);
    if (glovar->namesym && glovar->namesym->namestr == '_')
        LLVMSetVisibility(glovar->llvmvar, LLVMHiddenVisibility);
}

// Create mangled function name for overloaded function
char *genlMangleMethName(char *workbuf, FnDclNode *node) {
    // Use genned name if not an overloadable method
    if (!(node->flags & FlagMethProp))
        return node->genname;

    strcat(workbuf, node->genname);
    char *bufp = workbuf + strlen(workbuf);

    FnSigNode *fnsig = (FnSigNode *)node->vtype;
    int16_t cnt;
    INode **nodesp;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        *bufp++ = ':';
        bufp = itypeMangle(bufp, ((ITypedNode *)*nodesp)->vtype);
    }
    *bufp = '\0';

    return workbuf;
}

// Generate LLVMValueRef for a global function
void genlGloFnName(GenState *gen, FnDclNode *glofn) {
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

// Generate module's nodes
void genlModule(GenState *gen, ModuleNode *mod) {
    uint32_t cnt;
    INode **nodesp;

    // First generate the global variable LLVMValueRef for every global variable
    // This way forward references to global variables will work correctly
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        INode *nodep = *nodesp;
        if (nodep->tag == VarDclTag)
            genlGloVarName(gen, (VarDclNode *)nodep);
        else if (nodep->tag == FnDclTag)
            genlGloFnName(gen, (FnDclNode *)nodep);
    }

    // Generate the function's block or the variable's initialization value
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        INode *nodep = *nodesp;
        switch (nodep->tag) {
        case VarDclTag:
            if (((VarDclNode*)nodep)->value) {
                genlGloVar(gen, (VarDclNode*)nodep);
            }
            break;

        case FnDclTag:
            if (((FnDclNode*)nodep)->value) {
                genlFn(gen, (FnDclNode*)nodep);
            }
            break;

        // Generate allocator definition
        case AllocTag:
            genlType(gen, nodep);
            break;

        case ModuleTag:
            genlModule(gen, (ModuleNode*)nodep);
            break;

        default:
            // No need to generate type declarations: type uses will do so
            if (isTypeNode(nodep))
                break;
            assert(0 && "Invalid global area node");
        }
    }
}

void genlPackage(GenState *gen, ModuleNode *mod) {

    assert(mod->tag == ModuleTag);
    gen->module = LLVMModuleCreateWithNameInContext(gen->opt->srcname, gen->context);
    if (!gen->opt->release) {
        gen->dibuilder = LLVMCreateDIBuilder(gen->module);
        gen->difile = LLVMDIBuilderCreateFile(gen->dibuilder, "main.cone", 9, ".", 1);
        gen->compileUnit = LLVMDIBuilderCreateCompileUnit(gen->dibuilder, LLVMDWARFSourceLanguageC,
            gen->difile, "Cone compiler", 13, 0, "", 0, 0, "", 0, LLVMDWARFEmissionFull, 0, 0, 0);
    }
    genlModule(gen, mod);
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
void genmod(GenState *gen, ModuleNode *mod) {
    char *err;

    // Generate IR to LLVM IR
    genlPackage(gen, mod);

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
    if (gen->opt->print_llvmir && LLVMPrintModuleToFile(gen->module, fileMakePath(gen->opt->output, mod->lexer->fname, "preir"), &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit pre-ir file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Optimize the generated LLVM IR
    timerBegin(OptTimer);
    LLVMPassManagerRef passmgr = LLVMCreatePassManager();
    LLVMAddDemoteMemoryToRegisterPass(passmgr);        // Demote allocas to registers.
    LLVMAddInstructionCombiningPass(passmgr);        // Do simple "peephole" and bit-twiddling optimizations
    LLVMAddReassociatePass(passmgr);                // Reassociate expressions.
    LLVMAddGVNPass(passmgr);                        // Eliminate common subexpressions.
    LLVMAddCFGSimplificationPass(passmgr);            // Simplify the control flow graph
    if (gen->opt->release)
        LLVMAddFunctionInliningPass(passmgr);        // Function inlining
    LLVMRunPassManager(passmgr, gen->module);
    LLVMDisposePassManager(passmgr);

    // Serialize the LLVM IR, if requested
    if (gen->opt->print_llvmir && LLVMPrintModuleToFile(gen->module, fileMakePath(gen->opt->output, mod->lexer->fname, "ir"), &err) != 0) {
        errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
        LLVMDisposeMessage(err);
    }

    // Transform IR to target's ASM and OBJ
    timerBegin(CodeGenTimer);
    if (gen->machine)
        genlOut(fileMakePath(gen->opt->output, mod->lexer->fname, gen->opt->wasm? "wasm" : objext),
            gen->opt->print_asm? fileMakePath(gen->opt->output, mod->lexer->fname, gen->opt->wasm? "wat" : asmext) : NULL,
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
    gen->fn = NULL;
    gen->block = NULL;
    gen->loopstack = memAllocBlk(sizeof(GenLoopState)*GenLoopMax);
    gen->loopstackcnt = 0;
}

void genClose(GenState *gen) {
    LLVMDisposeTargetMachine(gen->machine);
}