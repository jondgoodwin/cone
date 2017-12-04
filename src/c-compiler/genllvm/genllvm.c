/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "genllvm.h"
#include "../shared/error.h"
#include "../pass/pass.h"
#include "../shared/symbol.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

#include <stdio.h>
#include <assert.h>

// Generate a term
LLVMValueRef genlTerm(genl_t *gen, AstNode *termnode) {
	if (termnode->asttype == ULitNode) {
		return LLVMConstInt(LLVMInt32TypeInContext(gen->context), ((ULitAstNode*)termnode)->uintlit, 0);
	}
	else if (termnode->asttype == FLitNode) {
		return LLVMConstReal(LLVMFloatTypeInContext(gen->context), ((FLitAstNode*)termnode)->floatlit);
	}
	else
		return NULL;
}

// Generate a return statement
void genlReturn(genl_t *gen, StmtExpAstNode *node) {
	if (node->exp != voidType)
		LLVMBuildRet(gen->builder, genlTerm(gen, node->exp));
	else
		LLVMBuildRetVoid(gen->builder);
}

// Generate a type value
LLVMTypeRef genlType(genl_t *gen, AstNode *typ) {
	switch (typ->asttype) {
	case IntType: case UintType:
	{
		switch (((NbrTypeAstNode*)typ)->nbytes) {
		case 1: return LLVMInt8TypeInContext(gen->context);
		case 2: return LLVMInt16TypeInContext(gen->context);
		case 4: return LLVMInt32TypeInContext(gen->context);
		case 8: return LLVMInt64TypeInContext(gen->context);
		}
	}
	case FloatType:
	{
		switch (((NbrTypeAstNode*)typ)->nbytes) {
		case 4: return LLVMFloatTypeInContext(gen->context);
		case 8: return LLVMDoubleTypeInContext(gen->context);
		}
	}
	case VoidType:
		return LLVMVoidTypeInContext(gen->context);

	default:
			return NULL;
	}
}

// Generate a function block
void genlFn(genl_t *gen, FnImplAstNode *fnnode) {
	AstNode **nodesp;
	uint32_t cnt;

	// fn sum(a i32, b i32) i32 {..} ==> sum, builder
	LLVMTypeRef param_types[] = { LLVMInt32TypeInContext(gen->context), LLVMInt32TypeInContext(gen->context) };
	LLVMTypeRef ret_type = LLVMFunctionType(genlType(gen, ((FnSigAstNode*)fnnode->vtype)->rettype), param_types, 0, 0);
	gen->fn = LLVMAddFunction(gen->module, fnnode->name->name, ret_type);
	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
	gen->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(gen->builder, entry);

	assert(fnnode->asttype == FnImplNode);
	for (nodesFor(fnnode->nodes, cnt, nodesp)) {
		switch ((*nodesp)->asttype) {
		case StmtExpNode:
			genlTerm(gen, ((StmtExpAstNode*)*nodesp)->exp); break;
		case ReturnNode:
			genlReturn(gen, (StmtExpAstNode*)*nodesp); break;
		}
	}

	LLVMDisposeBuilder(gen->builder);
}

// Generate module from AST
void genlModule(genl_t *gen, PgmAstNode *pgm) {
	uint32_t cnt;
	AstNode **nodesp;
	char *error=NULL;

	gen->module = LLVMModuleCreateWithNameInContext("my_module", gen->context);

	assert(pgm->asttype == PgmNode);
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == FnImplNode)
			genlFn(gen, (FnImplAstNode*)nodep);
	}

	LLVMVerifyModule(gen->module, LLVMReturnStatusAction, &error);
	if (error) {
		if (*error)
			errorMsg(ErrorGenErr, "Module verification failed:\n%s", error);
		LLVMDisposeMessage(error);
	}
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
	//if (LLVMWriteBitcodeToFile(mod, "sum.bc") != 0) {
		//errorMsg(ErrorGenErr, "Error writing bitcode to file");
	//}

	// or LLVMAssemblyFile
	if (LLVMTargetMachineEmitToFile(machine, mod, "sum.obj", LLVMObjectFile, &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
		LLVMDisposeMessage(err);
		return;
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(pass_opt_t *opt, PgmAstNode *pgmast) {
	genl_t gen;
	LLVMTargetMachineRef machine;

	gen.context = LLVMContextCreate();
	genlModule(&gen, pgmast);
	machine = genlCreateMachine(opt);
	if (machine)
		genlOut(gen.module, opt->triple, machine);

	LLVMDisposeModule(gen.module);
	LLVMDisposeTargetMachine(machine);
	LLVMContextDispose(gen.context);
}
