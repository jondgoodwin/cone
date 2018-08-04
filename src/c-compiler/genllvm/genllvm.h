/** Generator for LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef genllvm_h
#define genllvm_h

#include "../ir/ir.h"
#include "../coneopts.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>

typedef struct GenState {
	LLVMTargetDataRef datalayout;
	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMValueRef fn;
	LLVMBuilderRef builder;
	LLVMBasicBlockRef whilebeg;
	LLVMBasicBlockRef whileend;

	char *srcname;
} GenState;

void genllvm(ConeOptions *opt, ModuleAstNode *mod);
void genlFn(GenState *gen, FnDclAstNode *fnnode);
void genlGloVarName(GenState *gen, VarDclAstNode *glovar);
void genlGloFnName(GenState *gen, FnDclAstNode *glofn);

// genlstmt.c
LLVMBasicBlockRef genlInsertBlock(GenState *gen, char *name);
LLVMValueRef genlBlock(GenState *gen, BlockAstNode *blk);

// genlexpr.c
LLVMTypeRef genlType(GenState *gen, INode *typ);
LLVMValueRef genlExpr(GenState *gen, INode *termnode);

#endif
