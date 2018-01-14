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

#ifdef _WIN32
#define asmext "asm"
#define objext "obj"
#else
#define asmext "s"
#define objext "o"
#endif

// Generate parameter variable
void genlParmVar(genl_t *gen, NameDclAstNode *var) {
	assert(var->asttype == VarNameDclNode);
	// We always alloca in case variable is mutable or we want to take address of its value
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, (AstNode*)var), &var->namesym->namestr);
	LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, var->index), var->llvmvar);
}

// Generate a function
void genlFn(genl_t *gen, NameDclAstNode *fnnode) {
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
}

// Generate global variable
void genlGloVar(genl_t *gen, NameDclAstNode *varnode) {
	LLVMSetInitializer(varnode->llvmvar, genlExpr(gen, varnode->value));
}

// Generate LLVMValueRef for a global variable or function
void genlGloVarName(genl_t *gen, NameDclAstNode *glovar) {
	// Handle when it is just a global variable
	if (glovar->vtype->asttype != FnSig) {
		glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), &glovar->namesym->namestr);
		if (glovar->perm == immPerm)
			LLVMSetGlobalConstant(glovar->llvmvar, 1);
		return;
	}

	// Add function to the module
	glovar->llvmvar = LLVMAddFunction(gen->module, &glovar->namesym->namestr, genlType(gen, glovar->vtype));
}

// Generate module's nodes
void genlModule(genl_t *gen, PgmAstNode *pgm) {
	uint32_t cnt;
	AstNode **nodesp;
	char *error=NULL;

	assert(pgm->asttype == PgmNode);

	gen->module = LLVMModuleCreateWithNameInContext(gen->srcname, gen->context);

	// First generate the global variable LLVMValueRef for every global variable
	// This way forward references to global variables will work correctly
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == VarNameDclNode)
			genlGloVarName(gen, (NameDclAstNode *)nodep);
	}

	// Generate the function's block or the variable's initialization value
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == VarNameDclNode && ((NameDclAstNode*)nodep)->value) {
			if (((NameDclAstNode*)nodep)->vtype->asttype == FnSig)
				genlFn(gen, (NameDclAstNode*)nodep);
			else
				genlGloVar(gen, (NameDclAstNode*)nodep);
		}
	}

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
	if (!(machine = LLVMCreateTargetMachine(target, opt->triple, opt->cpu, opt->features, opt_level, reloc, LLVMCodeModelDefault)))
		errorMsg(ErrorGenErr, "Could not create target machine");
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
void genllvm(ConeOptions *opt, PgmAstNode *pgmast) {
	char *err;
	genl_t gen;
	LLVMTargetMachineRef machine;

	gen.srcname = pgmast->lexer->fname;
	gen.context = LLVMContextCreate();

	// Generate AST to IR
	genlModule(&gen, pgmast);

	// Serialize the LLVM IR, if requested
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, pgmast->lexer->fname, "preir"), &err) != 0) {
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
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, pgmast->lexer->fname, "ir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit ir file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Transform IR to target's ASM and OBJ
	machine = genlCreateMachine(opt);
	if (machine)
		genlOut(fileMakePath(opt->output, pgmast->lexer->fname, objext),
			opt->print_asm? fileMakePath(opt->output, pgmast->lexer->fname, asmext) : NULL,
			gen.module, opt->triple, machine);

	LLVMDisposeModule(gen.module);
	LLVMDisposeTargetMachine(machine);
	LLVMContextDispose(gen.context);
}
