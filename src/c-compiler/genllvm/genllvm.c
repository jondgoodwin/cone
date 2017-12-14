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
	else if (termnode->asttype == VarNameUseNode) {
		// Load from a global variable (generalize later for local variable if scope > 0)
		char *name = &((NameUseAstNode *)termnode)->dclnode->namesym->namestr;
		return LLVMBuildLoad(gen->builder, ((NameUseAstNode *)termnode)->dclnode->llvmvar, name);
	}
	else if (termnode->asttype == AssignNode) {
		AssignAstNode *node = (AssignAstNode*) termnode;
		char *lvalname = &((NameUseAstNode *)node->lval)->dclnode->namesym->namestr;
		LLVMValueRef glovar = LLVMGetNamedGlobal(gen->module, lvalname);
		return LLVMBuildStore(gen->builder, genlTerm(gen, node->rval), glovar);
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
	// If it's a name, resolve it to the actual type info
	case VtypeNameUseNode:
		return genlType(gen, ((NameUseAstNode *)typ)->dclnode->value);
	case IntNbrType: case UintNbrType:
	{
		switch (((NbrAstNode*)typ)->nbytes) {
		case 1: return LLVMInt8TypeInContext(gen->context);
		case 2: return LLVMInt16TypeInContext(gen->context);
		case 4: return LLVMInt32TypeInContext(gen->context);
		case 8: return LLVMInt64TypeInContext(gen->context);
		}
	}
	case FloatNbrType:
	{
		switch (((NbrAstNode*)typ)->nbytes) {
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

// Generate global variable
void genlGloVar(genl_t *gen, NameDclAstNode *varnode) {
	LLVMValueRef var;
	varnode->llvmvar = var = LLVMAddGlobal(gen->module, genlType(gen, varnode->vtype), &varnode->namesym->namestr);
	if (varnode->value) {
		LLVMSetInitializer(var, genlTerm(gen, varnode->value));
	}
}

// Generate a function block
void genlFn(genl_t *gen, NameDclAstNode *fnnode) {
	BlockAstNode *blk;
	AstNode **nodesp;
	uint32_t cnt;

	if (!fnnode->value)
		return;
	assert(fnnode->value->asttype == BlockNode);

	// Add function and its signature to module
	LLVMTypeRef param_types[] = { LLVMInt32TypeInContext(gen->context), LLVMInt32TypeInContext(gen->context) };
	LLVMTypeRef ret_type = LLVMFunctionType(genlType(gen, ((FnSigAstNode*)fnnode->vtype)->rettype), param_types, 0, 0);
	fnnode->llvmvar = gen->fn = LLVMAddFunction(gen->module, &fnnode->namesym->namestr, ret_type);

	// Attach block and builder to function
	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
	gen->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(gen->builder, entry);

	// Populate block with statements
	blk = (BlockAstNode *)fnnode->value;
	for (nodesFor(blk->nodes, cnt, nodesp)) {
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

	gen->module = LLVMModuleCreateWithNameInContext(gen->srcname, gen->context);

	assert(pgm->asttype == PgmNode);
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == VarNameDclNode) {
			if (((NameDclAstNode*)nodep)->vtype->asttype == FnSig)
				genlFn(gen, (NameDclAstNode*)nodep);
			else
				genlGloVar(gen, (NameDclAstNode*)nodep);
		}
	}

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
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, pgmast->lexer->fname, "ir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit obj file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Transform IR to target's ASM and OBJ
	machine = genlCreateMachine(opt);
	if (machine)
		genlOut(fileMakePath(opt->output, pgmast->lexer->fname, "obj"),
			opt->print_asm? fileMakePath(opt->output, pgmast->lexer->fname, "asm") : NULL,
			gen.module, opt->triple, machine);

	LLVMDisposeModule(gen.module);
	LLVMDisposeTargetMachine(machine);
	LLVMContextDispose(gen.context);
}
