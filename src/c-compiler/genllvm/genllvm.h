/** Generator for LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef genllvm_h
#define genllvm_h

#include "../ast/ast.h"
#include "../coneopts.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>

typedef struct genl_t {
	LLVMTargetDataRef datalayout;
	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMValueRef fn;
	LLVMBuilderRef builder;
	LLVMBasicBlockRef whilebeg;
	LLVMBasicBlockRef whileend;

	char *srcname;
} genl_t;

void genllvm(ConeOptions *opt, PgmAstNode *pgmast);
void genlFn(genl_t *gen, NameDclAstNode *fnnode);
void genlGloVarName(genl_t *gen, NameDclAstNode *glovar);

// genlstmt.c
LLVMBasicBlockRef genlInsertBlock(genl_t *gen, char *name);
LLVMValueRef genlBlock(genl_t *gen, BlockAstNode *blk);

// genlexpr.c
LLVMTypeRef genlType(genl_t *gen, AstNode *typ);
LLVMValueRef genlExpr(genl_t *gen, AstNode *termnode);

#endif
