/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/error.h"
#include "pass/pass.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

#include <stdio.h>
#include <assert.h>

// Generate a term
void genlTerm(AstNode *termnode) {
	if (termnode->asttype == ULitNode) {
		printf("OMG Found an integer %lld\n", ((ULitAstNode*)termnode)->uintlit);
	}
	else if (termnode->asttype == FLitNode) {
		printf("OMG Found a float %f\n", ((FLitAstNode*)termnode)->floatlit);
	}
}

// Generate a return statement
void genlReturn(StmtExpAstNode *node) {
	if (node->exp != voidType)
		genlTerm(node->exp);
}

// Generate a function block
void genlFn(FnImplAstNode *fnnode) {
	AstNode **nodesp;
	uint32_t cnt;

	assert(fnnode->asttype == FnImplNode);
	for (nodesFor(fnnode->nodes, cnt, nodesp)) {
		switch ((*nodesp)->asttype) {
		case StmtExpNode:
			genlTerm(((StmtExpAstNode*)*nodesp)->exp); break;
		case ReturnNode:
			genlReturn((StmtExpAstNode*)*nodesp); break;
		}
	}
}

// Generate module from AST
LLVMModuleRef genlMod(PgmAstNode *pgm) {
	uint32_t cnt;
	AstNode **nodesp;

	LLVMModuleRef mod = LLVMModuleCreateWithName("my_module");

	assert(pgm->asttype == PgmNode);
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == FnImplNode)
			genlFn((FnImplAstNode*)nodep);
	}

	// fn sum(a i32, b i32) i32 {..} ==> sum, builder
	LLVMTypeRef param_types[] = { LLVMInt32Type(), LLVMInt32Type() };
	LLVMTypeRef ret_type = LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
	LLVMValueRef sum = LLVMAddFunction(mod, "sum", ret_type);
	LLVMBasicBlockRef entry = LLVMAppendBasicBlock(sum, "entry");
	LLVMBuilderRef builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(builder, entry);

	// tmp = +(a, b)
	LLVMValueRef tmp = LLVMBuildAdd(builder, LLVMGetParam(sum, 0), LLVMGetParam(sum, 1), "tmp");

	// return tmp
	LLVMBuildRet(builder, tmp);

	LLVMDisposeBuilder(builder);

	char *error = NULL;
	LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
	LLVMDisposeMessage(error);
	return mod;
}

// Use provided options (triple, etc.) to creation a machine
LLVMTargetMachineRef genlCreateMachine(pass_opt_t *opt) {
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
void genlOut(LLVMModuleRef mod, char *triple, LLVMTargetMachineRef machine) {
	char *err;
	LLVMTargetDataRef dataref;
	char *layout;

	LLVMSetTarget(mod, triple);
	dataref = LLVMCreateTargetDataLayout(machine);
	layout = LLVMCopyStringRepOfTargetData(dataref);
	LLVMSetDataLayout(mod, layout);
	LLVMDisposeMessage(layout);

	// Write out bitcode to file
	if (LLVMWriteBitcodeToFile(mod, "sum.bc") != 0) {
		errorMsg(ErrorGenErr, "Error writing bitcode to file");
	}

	// or LLVMAssemblyFile
	if (LLVMTargetMachineEmitToFile(machine, mod, "sum.obj", LLVMObjectFile, &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
		LLVMDisposeMessage(err);
		return;
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(pass_opt_t *opt, PgmAstNode *pgm) {
	LLVMModuleRef mod;
	LLVMTargetMachineRef machine;

	mod = genlMod(pgm);
	machine = genlCreateMachine(opt);
	if (!machine)
		genlOut(mod, opt->triple, machine);

	LLVMDisposeModule(mod);
	LLVMDisposeTargetMachine(machine);
}
