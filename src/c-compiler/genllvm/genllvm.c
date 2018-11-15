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

void genlNamePrefix(INamedNode *name, char *workbuf) {
	if (name->namesym==NULL)
		return;
	genlNamePrefix(name->owner, workbuf);
	strcat(workbuf, &name->namesym->namestr);
	strcat(workbuf, ":");
}

// Create and return globally unique name, mangled as necessary
char *genlGlobalName(INamedNode *name) {
	// Is mangling necessary? Only if we need namespace qualifier or method might be overloaded
	if ((name->flags & FlagExtern) || !(name->tag == FnDclTag && (name->flags & FlagMethProp)) && !name->owner->namesym)
		return &name->namesym->namestr;

	char workbuf[2048] = { '\0' };
	genlNamePrefix(name->owner, workbuf);
	strcat(workbuf, &name->namesym->namestr);

    // Mangle method name with parameter types so that overloaded methods have distinct names
	if (name->flags & FlagMethProp) {
		FnSigNode *fnsig = (FnSigNode *)name->vtype;
		char *bufp = workbuf + strlen(workbuf);
		int16_t cnt;
		INode **nodesp;
		for (nodesFor(fnsig->parms, cnt, nodesp)) {
			*bufp++ = ':';
			bufp = itypeMangle(bufp, ((ITypedNode *)*nodesp)->vtype);
		}
		*bufp = '\0';
	}
	return memAllocStr(workbuf, strlen(workbuf));
}

// Generate LLVMValueRef for a global variable
void genlGloVarName(GenState *gen, VarDclNode *glovar) {
	glovar->llvmvar = LLVMAddGlobal(gen->module, genlType(gen, glovar->vtype), genlGlobalName((INamedNode*)glovar));
	if (permIsSame(glovar->perm, (INode*) immPerm))
		LLVMSetGlobalConstant(glovar->llvmvar, 1);
}

// Generate LLVMValueRef for a global function
void genlGloFnName(GenState *gen, FnDclNode *glofn) {
    // Add function to the module
    if (glofn->value == NULL || glofn->value->tag != IntrinsicTag) {
        glofn->llvmvar = LLVMAddFunction(gen->module, genlGlobalName((INamedNode*)glofn), genlType(gen, glofn->vtype));
        if (glofn->flags & FlagSystem) {
            LLVMSetFunctionCallConv(glofn->llvmvar, LLVMX86StdcallCallConv);
            LLVMSetDLLStorageClass(glofn->llvmvar, LLVMDLLImportStorageClass);
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
	char *error = NULL;

	assert(mod->tag == ModuleTag);
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

// Generate IR nodes into LLVM IR using LLVM
void genllvm(ConeOptions *opt, ModuleNode *mod) {
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

	// Generate IR to LLVM IR
	genlPackage(&gen, mod);

	// Serialize the LLVM IR, if requested
	if (opt->print_llvmir && LLVMPrintModuleToFile(gen.module, fileMakePath(opt->output, mod->lexer->fname, "preir"), &err) != 0) {
		errorMsg(ErrorGenErr, "Could not emit pre-ir file: %s", err);
		LLVMDisposeMessage(err);
	}

	// Optimize the generated LLVM IR
	LLVMPassManagerRef passmgr = LLVMCreatePassManager();
	LLVMAddDemoteMemoryToRegisterPass(passmgr);	// Promote allocas to registers.
	LLVMAddInstructionCombiningPass(passmgr);		// Do simple "peephole" and bit-twiddling optimizations
	LLVMAddReassociatePass(passmgr);				// Reassociate expressions.
	LLVMAddGVNPass(passmgr);						// Eliminate common subexpressions.
	LLVMAddCFGSimplificationPass(passmgr);			// Simplify the control flow graph
    if (opt->release)
	    LLVMAddFunctionInliningPass(passmgr);		// Function inlining
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
