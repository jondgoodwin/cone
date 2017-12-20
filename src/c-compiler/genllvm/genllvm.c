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

// Generate a type value
LLVMTypeRef genlType(genl_t *gen, AstNode *typ) {
	switch (typ->asttype) {
	// If it's a name declaration (e.g., parm), resolve it to the actual type info
	case VarNameDclNode:
		return genlType(gen, ((NameDclAstNode *)typ)->vtype);
	// If it's a name, resolve it to the actual type info
	case VtypeNameUseNode:
		return genlType(gen, ((NameUseAstNode *)typ)->dclnode->value);
	case IntNbrType: case UintNbrType:
	{
		switch (((NbrAstNode*)typ)->bits) {
		case 8: return LLVMInt8TypeInContext(gen->context);
		case 16: return LLVMInt16TypeInContext(gen->context);
		case 32: return LLVMInt32TypeInContext(gen->context);
		case 64: return LLVMInt64TypeInContext(gen->context);
		}
	}
	case FloatNbrType:
	{
		switch (((NbrAstNode*)typ)->bits) {
		case 32: return LLVMFloatTypeInContext(gen->context);
		case 64: return LLVMDoubleTypeInContext(gen->context);
		}
	}
	case VoidType:
		return LLVMVoidTypeInContext(gen->context);

	default:
		return NULL;
	}
}

// Generate a term
LLVMValueRef genlTerm(genl_t *gen, AstNode *termnode) {
	switch (termnode->asttype) {
	case ULitNode:
		return LLVMConstInt(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((ULitAstNode*)termnode)->uintlit, 0);
	case FLitNode:
		return LLVMConstReal(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((FLitAstNode*)termnode)->floatlit);
	case VarNameUseNode:
	{
		// Load from a variable. If pointer, do a load otherwise assume it is the (immutable) value
		char *name = &((NameUseAstNode *)termnode)->dclnode->namesym->namestr;
		LLVMValueRef varval = ((NameUseAstNode *)termnode)->dclnode->llvmvar;
		if (LLVMGetTypeKind(LLVMTypeOf(varval)) == LLVMPointerTypeKind)
			return LLVMBuildLoad(gen->builder, varval, name);
		else
			return varval;
	}
	case FnCallNode:
	{
		FnCallAstNode *fncall = (FnCallAstNode*)termnode;
		NameUseAstNode *fnuse = (NameUseAstNode *)fncall->fn;
		char *fnname = &fnuse->dclnode->namesym->namestr;
		LLVMValueRef *fnargs = (LLVMValueRef*)memAllocBlk(fncall->parms->used * sizeof(LLVMValueRef*));
		LLVMValueRef *fnarg = fnargs;
		AstNode **nodesp;
		uint32_t cnt;
		for (nodesFor(fncall->parms, cnt, nodesp))
			*fnarg++ = genlTerm(gen, *nodesp);
		return LLVMBuildCall(gen->builder, LLVMGetNamedFunction(gen->module, fnname), fnargs, fncall->parms->used, "");
	}
	case AssignNode:
	{
		AssignAstNode *node = (AssignAstNode*)termnode;
		char *lvalname = &((NameUseAstNode *)node->lval)->dclnode->namesym->namestr;
		LLVMValueRef glovar = LLVMGetNamedGlobal(gen->module, lvalname);
		return LLVMBuildStore(gen->builder, genlTerm(gen, node->rval), glovar);
	}
	case CastNode:
	{
		CastAstNode *node = (CastAstNode*)termnode;
		NbrAstNode *fromtype = (NbrAstNode *)typeGetVtype(node->exp);
		NbrAstNode *totype = (NbrAstNode *)typeGetVtype(node->vtype);
		if (totype->asttype == UintNbrType) {
			if (fromtype->asttype == FloatNbrType)
				return LLVMBuildFPToUI(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits < fromtype->bits)
				return LLVMBuildTrunc(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits > fromtype->bits)
				return LLVMBuildZExt(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else
				return LLVMBuildBitCast(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		}
		else if (totype->asttype == IntNbrType) {
			if (fromtype->asttype == FloatNbrType)
				return LLVMBuildFPToSI(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits < fromtype->bits)
				return LLVMBuildTrunc(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits > fromtype->bits) {
				if (fromtype->asttype == IntNbrType)
					return LLVMBuildSExt(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
				else
					return LLVMBuildZExt(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			}
			else
				return LLVMBuildBitCast(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		}
		else {
			if (fromtype->asttype == IntNbrType)
				return LLVMBuildSIToFP(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (fromtype->asttype == UintNbrType)
				return LLVMBuildUIToFP(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits < fromtype->bits)
				return LLVMBuildFPTrunc(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else if (totype->bits > fromtype->bits)
				return LLVMBuildFPExt(gen->builder, genlTerm(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		}
	}
	default:
		printf("Unknown AST node to genLTerm!");
		return NULL;
	}
}

// Generate a return statement
void genlReturn(genl_t *gen, StmtExpAstNode *node) {
	if (node->exp != voidType)
		LLVMBuildRet(gen->builder, genlTerm(gen, node->exp));
	else
		LLVMBuildRetVoid(gen->builder);
}

// Generate LLVMValueRef for a global variable or function
void genlGloVarName(genl_t *gen, NameDclAstNode *glovar) {
	// Handle when it is just a global variable
	if (glovar->vtype->asttype != FnSig) {
		glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), &glovar->namesym->namestr);
		return;
	}

	// Handle when it is a function, building info from function signature
	FnSigAstNode *fnsig = (FnSigAstNode*)glovar->vtype;
	LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
	LLVMTypeRef *parm = param_types;
	SymNode *nodesp;
	uint32_t cnt;
	for (inodesFor(fnsig->parms, cnt, nodesp)) {
		assert(nodesp->node->asttype == VarNameDclNode);
		*parm++ = genlType(gen, nodesp->node);
	}
	LLVMTypeRef ret_type = LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
	glovar->llvmvar = LLVMAddFunction(gen->module, &glovar->namesym->namestr, ret_type);
}

// Generate global variable
void genlGloVar(genl_t *gen, NameDclAstNode *varnode) {
	LLVMSetInitializer(varnode->llvmvar, genlTerm(gen, varnode->value));
}

// Generate a function block
void genlFn(genl_t *gen, NameDclAstNode *fnnode) {
	FnSigAstNode *fnsig = (FnSigAstNode*)fnnode->vtype;
	BlockAstNode *blk;
	AstNode **nodesp;
	uint32_t cnt;

	assert(fnnode->value->asttype == BlockNode);
	gen->fn = fnnode->llvmvar;

	// Generate LLVMValueRef's for all parameters, so we can use them as local vars in code
	SymNode *inodesp;
	for (inodesFor(fnsig->parms, cnt, inodesp)) {
		assert(inodesp->node->asttype == VarNameDclNode);
		NameDclAstNode *var = (NameDclAstNode*)inodesp->node;
		var->llvmvar = LLVMGetParam(gen->fn, var->index);
	}

	// Attach block and builder to function
	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
	gen->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(gen->builder, entry);

	// Populate block with statements
	blk = (BlockAstNode *)fnnode->value;
	for (nodesFor(blk->stmts, cnt, nodesp)) {
		switch ((*nodesp)->asttype) {
		case StmtExpNode:
			genlTerm(gen, ((StmtExpAstNode*)*nodesp)->exp); break;
		case ReturnNode:
			genlReturn(gen, (StmtExpAstNode*)*nodesp); break;
		}
	}

	LLVMDisposeBuilder(gen->builder);
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
