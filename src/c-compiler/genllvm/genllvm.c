/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../shared/symbol.h"
#include "../shared/fileio.h"
#include "genllvm.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/Scalar.h>

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
void genlParmVar(GenState *gen, NameDclAstNode *var) {
	assert(var->asttype == VarNameDclNode);
	// We always alloca in case variable is mutable or we want to take address of its value
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, var->vtype), &var->namesym->namestr);
	LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, var->index), var->llvmvar);
}

// Generate a function
void genlFn(GenState *gen, NameDclAstNode *fnnode) {
	LLVMValueRef svfn = gen->fn;
	LLVMBuilderRef svbuilder = gen->builder;
	FnSigAstNode *fnsig = (FnSigAstNode*)fnnode->vtype;

	assert(fnnode->value->asttype == BlockNode);
	gen->fn = fnnode->llvmvar;

	// Attach block and builder to function
	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
	gen->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(gen->builder, entry);

	// Generate LLVMValueRef's for all parameters, so we can use them as local vars in code
	uint32_t cnt;
	SymNode *inodesp;
	for (inodesFor(fnsig->parms, cnt, inodesp))
		genlParmVar(gen, (NameDclAstNode*)inodesp->node);

	// Generate the function's code (always a block)
	genlBlock(gen, (BlockAstNode *)fnnode->value);

	LLVMDisposeBuilder(gen->builder);

	gen->builder = svbuilder;
	gen->fn = svfn;
}

// Generate global variable
void genlGloVar(GenState *gen, NameDclAstNode *varnode) {
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
	// Is mangling necessary? Only if we need namespace qualifier or function might be overloaded
	if (!(name->flags & FlagMangleParms) && !name->owner->namesym)
		return &name->namesym->namestr;

	char workbuf[2048] = { '\0' };
	genlNamePrefix(name->owner, workbuf);
	strcat(workbuf, &name->namesym->namestr);
	if (name->flags & FlagMangleParms) {
		FnSigAstNode *fnsig = (FnSigAstNode *)name->vtype;
		char *bufp = workbuf + strlen(workbuf);
		int16_t cnt;
		SymNode *nodesp;
		for (inodesFor(fnsig->parms, cnt, nodesp)) {
			*bufp++ = ':';
			bufp = typeMangle(bufp, ((TypedAstNode *)nodesp->node)->vtype);
		}
		*bufp = '\0';
	}
	return memAllocStr(workbuf, strlen(workbuf));
}

// Generate LLVMValueRef for a global variable or function
void genlGloVarName(GenState *gen, NameDclAstNode *glovar) {

	// Handle when it is just a global variable
	if (glovar->vtype->asttype != FnSig) {
		glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), genlGlobalName((NamedAstNode*)glovar));
		if (glovar->perm == immPerm)
			LLVMSetGlobalConstant(glovar->llvmvar, 1);
		return;
	}

	// Add function to the module
	glovar->llvmvar = LLVMAddFunction(gen->module, genlGlobalName((NamedAstNode*)glovar), genlType(gen, glovar->vtype));
}

// Generate module's nodes
void genlModule(GenState *gen, ModuleAstNode *mod) {
	uint32_t cnt;
	AstNode **nodesp;

	// First generate the global variable LLVMValueRef for every global variable
	// This way forward references to global variables will work correctly
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == VarNameDclNode)
			genlGloVarName(gen, (NameDclAstNode *)nodep);
	}

	// Generate the function's block or the variable's initialization value
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		switch (nodep->asttype) {
		case VarNameDclNode:
			if (((NameDclAstNode*)nodep)->value) {
				if (((NameDclAstNode*)nodep)->vtype->asttype == FnSig)
					genlFn(gen, (NameDclAstNode*)nodep);
				else
					genlGloVar(gen, (NameDclAstNode*)nodep);
			}
			break;

		// No need to generate type declarations: type uses will do so
		case VtypeNameDclNode:
			break;

		// Generate allocator definition
		case AllocNameDclNode:
			genlType(gen, nodep);
			break;

		case ModuleNode:
			genlModule(gen, (ModuleAstNode*)nodep);
			break;

		default:
			assert(0 && "Invalid global area node");
		}
	}
}

void genlPackage(GenState *gen, ModuleAstNode *mod) {
	char *error = NULL;

	assert(mod->asttype == ModuleNode);
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
	gen.context = LLVMContextCreate();

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
	LLVMRunPassManager(passmgr, gen.module);
	LLVMDisposePassManager(passmgr);

	// Serialize the LLVM IR, if requested
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, mod->lexer->fname, "ir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Transform IR to target's ASM and OBJ
	if (machine)
		genlOut(fileMakePath(opt->output, mod->lexer->fname, objext),
			opt->print_asm? fileMakePath(opt->output, mod->lexer->fname, asmext) : NULL,
			gen.module, opt->triple, machine);

	LLVMDisposeModule(gen.module);
	LLVMContextDispose(gen.context);
	LLVMDisposeTargetMachine(machine);
}
