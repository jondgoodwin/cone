/** Code generation via LLVM
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
#include <llvm-c/Transforms/IPO.h>
#if LLVM_VERSION_MAJOR >= 7
#include "llvm/Transforms/Utils.h"
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
void genlParmVar(GenState *gen, VarDclAstNode *var) {
	assert(var->asttype == VarDclTag);
	// We always alloca in case variable is mutable or we want to take address of its value
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, var->vtype), &var->namesym->namestr);
	LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, var->index), var->llvmvar);
}

// Generate a function
void genlFn(GenState *gen, FnDclAstNode *fnnode) {
    if (fnnode->value->asttype == IntrinsicTag)
        return;

	LLVMValueRef svfn = gen->fn;
	LLVMBuilderRef svbuilder = gen->builder;
	FnSigAstNode *fnsig = (FnSigAstNode*)fnnode->vtype;

	assert(fnnode->value->asttype == BlockTag);
	gen->fn = fnnode->llvmvar;

	// Attach block and builder to function
	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
	gen->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(gen->builder, entry);

	// Generate LLVMValueRef's for all parameters, so we can use them as local vars in code
	uint32_t cnt;
	INode **nodesp;
	for (nodesFor(fnsig->parms, cnt, nodesp))
		genlParmVar(gen, (VarDclAstNode*)*nodesp);

	// Generate the function's code (always a block)
	genlBlock(gen, (BlockAstNode *)fnnode->value);

	LLVMDisposeBuilder(gen->builder);

	gen->builder = svbuilder;
	gen->fn = svfn;
}

// Generate global variable
void genlGloVar(GenState *gen, VarDclAstNode *varnode) {
	LLVMSetInitializer(varnode->llvmvar, genlExpr(gen, varnode->value));
}

void genlNamePrefix(NamedAstNode *name, char *workbuf) {
	if (name->namesym==NULL)
		return;
	genlNamePrefix(name->owner, workbuf);
	strcat(workbuf, &name->namesym->namestr);
	strcat(workbuf, ":");
}

// Create and return globally unique name, mangled as necessary
char *genlGlobalName(NamedAstNode *name) {
	// Is mangling necessary? Only if we need namespace qualifier or method might be overloaded
	if ((name->flags & FlagExtern) || !(name->asttype == FnDclTag && (name->flags & FlagMethProp)) && !name->owner->namesym)
		return &name->namesym->namestr;

	char workbuf[2048] = { '\0' };
	genlNamePrefix(name->owner, workbuf);
	strcat(workbuf, &name->namesym->namestr);

    // Mangle method name with parameter types so that overloaded methods have distinct names
	if (name->flags & FlagMethProp) {
		FnSigAstNode *fnsig = (FnSigAstNode *)name->vtype;
		char *bufp = workbuf + strlen(workbuf);
		int16_t cnt;
		INode **nodesp;
		for (nodesFor(fnsig->parms, cnt, nodesp)) {
			*bufp++ = ':';
			bufp = typeMangle(bufp, ((TypedAstNode *)*nodesp)->vtype);
		}
		*bufp = '\0';
	}
	return memAllocStr(workbuf, strlen(workbuf));
}

// Generate LLVMValueRef for a global variable
void genlGloVarName(GenState *gen, VarDclAstNode *glovar) {
	glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), genlGlobalName((NamedAstNode*)glovar));
	if (glovar->perm == immPerm)
		LLVMSetGlobalConstant(glovar->llvmvar, 1);
}

// Generate LLVMValueRef for a global function
void genlGloFnName(GenState *gen, FnDclAstNode *glofn) {
    // Add function to the module
    if (glofn->value == NULL || glofn->value->asttype != IntrinsicTag)
        glofn->llvmvar = LLVMAddFunction(gen->module, genlGlobalName((NamedAstNode*)glofn), genlType(gen, glofn->vtype));
}

// Generate module's nodes
void genlModule(GenState *gen, ModuleAstNode *mod) {
	uint32_t cnt;
	INode **nodesp;

	// First generate the global variable LLVMValueRef for every global variable
	// This way forward references to global variables will work correctly
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		INode *nodep = *nodesp;
        if (nodep->asttype == VarDclTag)
            genlGloVarName(gen, (VarDclAstNode *)nodep);
        else if (nodep->asttype == FnDclTag)
            genlGloFnName(gen, (FnDclAstNode *)nodep);
	}

	// Generate the function's block or the variable's initialization value
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		INode *nodep = *nodesp;
		switch (nodep->asttype) {
        case VarDclTag:
            if (((VarDclAstNode*)nodep)->value) {
                genlGloVar(gen, (VarDclAstNode*)nodep);
            }
            break;

        case FnDclTag:
			if (((FnDclAstNode*)nodep)->value) {
        		genlFn(gen, (FnDclAstNode*)nodep);
			}
			break;

		// Generate allocator definition
		case AllocTag:
			genlType(gen, nodep);
			break;

		case ModuleTag:
			genlModule(gen, (ModuleAstNode*)nodep);
			break;

		default:
            // No need to generate type declarations: type uses will do so
            if (isTypeNode(nodep))
                break;
            assert(0 && "Invalid global area node");
		}
	}
}

void genlPackage(GenState *gen, ModuleAstNode *mod) {
	char *error = NULL;

	assert(mod->asttype == ModuleTag);
	gen->module = LLVMModuleCreateWithNameInContext(gen->srcname, gen->context);
	genlModule(gen, mod);
	// Verify generated IR
	LLVMVerifyModule(gen->module, LLVMReturnStatusAction, &error);
	if (error) {
		if (*error)
			errorMsg(ErrorGenErr, "Module verification failed:\n%s", error);
		LLVMDisposeMessage(error);
	}
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

// Generate AST into LLVM IR using LLVM
void genllvm(ConeOptions *opt, ModuleAstNode *mod) {
	char *err;
	GenState gen;

	LLVMTargetMachineRef machine = genlCreateMachine(opt);
	if (!machine)
		exit(ExitOpts);

	// Obtain data layout info, particularly pointer sizes
	gen.datalayout = LLVMCreateTargetDataLayout(machine);
	opt->ptrsize = LLVMPointerSize(gen.datalayout) << 3;
	usizeType->bits = isizeType->bits = opt->ptrsize;

	gen.srcname = mod->lexer->fname;
	gen.context = LLVMGetGlobalContext(); // LLVM inlining bugs prevent use of LLVMContextCreate();

	// Generate AST to IR
	genlPackage(&gen, mod);

	// Serialize the LLVM IR, if requested
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, mod->lexer->fname, "preir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit pre-ir file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Optimize the generated LLVM IR
	LLVMPassManagerRef passmgr = LLVMCreatePassManager();
	LLVMAddPromoteMemoryToRegisterPass(passmgr);	// Promote allocas to registers.
	LLVMAddInstructionCombiningPass(passmgr);		// Do simple "peephole" and bit-twiddling optimizations
	LLVMAddReassociatePass(passmgr);				// Reassociate expressions.
	LLVMAddGVNPass(passmgr);						// Eliminate common subexpressions.
	LLVMAddCFGSimplificationPass(passmgr);			// Simplify the control flow graph
	LLVMAddFunctionInliningPass(passmgr);			// Function inlining
	LLVMRunPassManager(passmgr, gen.module);
	LLVMDisposePassManager(passmgr);

	// Serialize the LLVM IR, if requested
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, mod->lexer->fname, "ir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Transform IR to target's ASM and OBJ
	if (machine)
		genlOut(fileMakePath(opt->output, mod->lexer->fname, opt->wasm? "wasm" : objext),
			opt->print_asm? fileMakePath(opt->output, mod->lexer->fname, opt->wasm? "wat" : asmext) : NULL,
			gen.module, opt->triple, machine);

	LLVMDisposeModule(gen.module);
	// LLVMContextDispose(gen.context);  // Only need if we created a new context
	LLVMDisposeTargetMachine(machine);
}
